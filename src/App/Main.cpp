// openera: command-line launcher for unpacked title folders.
#include <array>
#include <bit>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>

#include "App/LogWindow.h"
#include "App/Settings.h"
#include "App/TitleLauncher.h"
#include "Core/Config.h"
#include "Core/Logger.h"
#include "Graphics/AdapterSelection.h"
#include "Loader/GameConfig.h"

// Ask hybrid-graphics drivers for the high-performance GPU, as NVIDIA (Optimus
// rendering policies) and AMD (PowerXpress) document. The drivers look for these
// exports in the process executable, which is openera.exe rather than the title.
extern "C"
{
    // NOLINTBEGIN(readability-identifier-naming): names fixed by the drivers.
    __declspec(dllexport) extern const DWORD NvOptimusEnablement = 1;
    __declspec(dllexport) extern const int AmdPowerXpressRequestHighPerformance = 1;
    // NOLINTEND(readability-identifier-naming)
}

namespace
{
using OpenEra::App::LaunchError;
using OpenEra::Core::Config;
using OpenEra::Core::ConfigIssue;
using OpenEra::Core::Logger;
using OpenEra::Core::LogLevel;
namespace Setting = OpenEra::App::Setting;

// Exit codes. After a title runs, openera exits with the title's own exit code.
constexpr int kExitSuccess = 0;
constexpr int kExitFailure = 1; // Unexpected error.
constexpr int kExitUsage = 2;
constexpr int kExitInput = 3;  // Not a folder, or a package.
constexpr int kExitConfig = 4; // MicrosoftGame.config missing or unusable, or no such executable.
constexpr int kExitLoad = 5;   // Loading or linking failed.
constexpr int kExitStart = 6;  // Start-up or the entry point failed.

constexpr std::string_view kUsage = R"(Usage:
  openera run <title folder> [options] [-- <title arguments>]
                                         Load and start a title
  openera info <title folder>            Show what MicrosoftGame.config describes
  openera config [<title folder>]        List every setting, its value and where it came from

The title folder is an unpacked title: the folder containing MicrosoftGame.config.

Settings come from %LOCALAPPDATA%\OpenEra\openera.toml, then the title's own
%LOCALAPPDATA%\OpenEra\Titles\<identity>\openera.toml, then the command line.

Options:
  --set <key>=<value>   Override a setting (see openera config)
  --config <path>       Read user settings from this file instead
  --exe <Id>            Executable to start (title.executable)
  --no-start            Load and link only
  --gpu <choice>        auto, nvidia, amd, intel or an adapter index (graphics.gpu)
  --log-level <level>   trace, info, warn or error (log.level)
  --log-file <path>     Also write the log to a file (log.file)
  --log-window          Also show the log in a window (log.window)
  --                    Pass the rest of the command line to the title
)";

struct CommandLine
{
    std::string Command;
    std::optional<std::filesystem::path> Folder;
    std::optional<std::filesystem::path> ConfigFile;
    std::vector<std::string> Overrides;       // key=value, in command-line order.
    std::vector<std::wstring> TitleArguments; // After --, for the title's own command line.
    bool Start = true;
};

std::string Narrow(const std::wstring& text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
    {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), length, nullptr, nullptr);
    result.pop_back(); // Terminator.
    return result;
}

std::string Assign(std::string_view key, const std::string& text)
{
    return std::format("{}={}", key, OpenEra::Core::FormatSettingValue(text));
}

std::optional<CommandLine> ParseCommandLine(const std::vector<std::wstring>& arguments)
{
    if (arguments.empty())
    {
        return std::nullopt;
    }
    CommandLine line;
    line.Command = Narrow(arguments.at(0));
    if (line.Command != "run" && line.Command != "info" && line.Command != "config")
    {
        return std::nullopt;
    }
    for (size_t index = 1; index < arguments.size(); ++index)
    {
        const std::string option = Narrow(arguments.at(index));
        const bool hasValue = index + 1 < arguments.size();
        if (option == "--")
        {
            for (size_t rest = index + 1; rest < arguments.size(); ++rest)
            {
                line.TitleArguments.push_back(arguments.at(rest));
            }
            break;
        }
        if (!option.starts_with("--") && !line.Folder)
        {
            line.Folder = arguments.at(index);
        }
        else if (option == "--no-start")
        {
            line.Start = false;
        }
        else if (option == "--log-window")
        {
            line.Overrides.push_back(std::string(Setting::kLogWindow) + "=true");
        }
        else if (option == "--set" && hasValue)
        {
            line.Overrides.push_back(Narrow(arguments.at(++index)));
        }
        else if (option == "--config" && hasValue)
        {
            line.ConfigFile = arguments.at(++index);
        }
        else if (option == "--exe" && hasValue)
        {
            line.Overrides.push_back(Assign(Setting::kExecutable, Narrow(arguments.at(++index))));
        }
        else if (option == "--gpu" && hasValue)
        {
            line.Overrides.push_back(Assign(Setting::kGpu, Narrow(arguments.at(++index))));
        }
        else if (option == "--log-file" && hasValue)
        {
            line.Overrides.push_back(Assign(Setting::kLogFile, Narrow(arguments.at(++index))));
        }
        else if (option == "--log-level" && hasValue)
        {
            line.Overrides.push_back(Assign(Setting::kLogLevel, Narrow(arguments.at(++index))));
        }
        else
        {
            return std::nullopt;
        }
    }
    if (line.Command != "config" && !line.Folder)
    {
        return std::nullopt;
    }
    return line;
}

constexpr int ExitCodeFor(LaunchError error) noexcept
{
    switch (error)
    {
    case LaunchError::None:
        return kExitSuccess;
    case LaunchError::NotAFolder:
    case LaunchError::PackagedInput:
        return kExitInput;
    case LaunchError::NoConfig:
    case LaunchError::BadConfig:
    case LaunchError::NoExecutable:
    case LaunchError::BadExecutablePath:
        return kExitConfig;
    case LaunchError::LoadFailed:
        return kExitLoad;
    case LaunchError::StartFailed:
        return kExitStart;
    }
    return kExitFailure;
}

void Append(std::vector<ConfigIssue>& issues, std::vector<ConfigIssue> more)
{
    issues.insert(issues.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
}

struct LoadedSettings
{
    std::vector<ConfigIssue> Issues;
    std::filesystem::path UserFile;
    std::optional<std::filesystem::path> TitleFile;
};

// Defaults, the user's file, the title's file, then the command line.
LoadedSettings LoadSettings(Config& config, const CommandLine& line)
{
    LoadedSettings loaded;
    loaded.UserFile = line.ConfigFile.value_or(OpenEra::App::UserConfigPath());
    Append(loaded.Issues, config.LoadFile(loaded.UserFile));
    if (line.Folder)
    {
        // Only the identity is needed here; a broken config is reported when the title runs.
        Logger quiet;
        const auto title = OpenEra::App::ReadTitleConfig(*line.Folder, quiet);
        if (title.Config)
        {
            loaded.TitleFile = OpenEra::App::TitleDataDirectory(title.Config->IdentityName) / "openera.toml";
            Append(loaded.Issues, config.LoadFile(*loaded.TitleFile));
        }
    }
    for (const auto& assignment : line.Overrides)
    {
        Append(loaded.Issues, config.ApplyOverride(assignment));
    }
    return loaded;
}

LogLevel LevelOf(const Config& config) noexcept
{
    try
    {
        const auto& level = config.GetString(Setting::kLogLevel);
        if (level == "trace")
        {
            return LogLevel::Trace;
        }
        if (level == "warn")
        {
            return LogLevel::Warn;
        }
        if (level == "error")
        {
            return LogLevel::Error;
        }
    }
    catch (const std::exception&)
    {
        return LogLevel::Info;
    }
    return LogLevel::Info;
}

int ShowInfo(const std::filesystem::path& folder, Logger& logger)
{
    const auto result = OpenEra::App::ReadTitleConfig(folder, logger);
    if (!result.Config)
    {
        return ExitCodeFor(result.Error);
    }
    const auto& config = *result.Config;
    const auto* selected = OpenEra::Loader::SelectExecutable(config);
    std::cout << std::format(
        "Title:        {} {}\nPublisher:    {}\nTitle ID:     {}\nDisplay name: {}\nExecutables:\n",
        config.IdentityName, config.Version, config.Publisher, config.TitleId, config.DisplayName);
    for (const auto& executable : config.Executables)
    {
        std::cout << std::format("  {} {}  Id={}  TargetDeviceFamily={}\n", &executable == selected ? '*' : ' ',
                                 executable.Name, executable.Id, executable.TargetDeviceFamily);
    }
    std::cout << "(* starts by default)\n";
    return kExitSuccess;
}

int ShowSettings(const Config& config, const LoadedSettings& loaded)
{
    std::cout << std::format("User settings:  {}\n", loaded.UserFile.string());
    if (loaded.TitleFile)
    {
        std::cout << std::format("Title settings: {}\n", loaded.TitleFile->string());
    }
    for (const auto& setting : config.Schema())
    {
        std::cout << std::format("\n{} = {}  ({})\n    {}\n", setting.Key, config.FormatValue(setting.Key),
                                 config.SourceOf(setting.Key), setting.Description);
        std::string details = std::format("    {}, default {}", OpenEra::Core::ToString(setting.Type),
                                          OpenEra::Core::FormatSettingValue(setting.Default));
        if (!setting.Choices.empty())
        {
            details += "; one of";
            for (const auto& choice : setting.Choices)
            {
                details += " " + choice;
            }
        }
        std::cout << details << '\n';
    }
    return kExitSuccess;
}

// With the log in its own window, a console that only openera uses (one
// Windows opened for it, rather than the terminal it was started from) just
// shows a blank window, so let it go.
void DetachOwnConsole() noexcept
{
    constexpr DWORD kProcessSlots = 2;
    std::array<DWORD, kProcessSlots> processes{};
    if (GetConsoleProcessList(processes.data(), kProcessSlots) == 1)
    {
        FreeConsole();
    }
}

int Run(const CommandLine& line, const Config& config, Logger& logger)
{
    OpenEra::App::LaunchOptions options;
    options.Folder = *line.Folder;
    options.ExecutableId = config.GetString(Setting::kExecutable);
    options.Start = line.Start;
    options.Arguments = line.TitleArguments;
    options.OverlayDirectory = std::filesystem::path(
        std::u8string(config.GetString(Setting::kOverlay).begin(), config.GetString(Setting::kOverlay).end()));
    const auto& gpu = config.GetString(Setting::kGpu);
    const auto preference = OpenEra::Graphics::ParseGpuPreference(gpu);
    if (!preference)
    {
        logger.Warn("Settings: {} \"{}\" isn't auto, nvidia, amd, intel or an adapter index; using auto", Setting::kGpu,
                    gpu);
    }
    OpenEra::Graphics::SetGpuPreference(preference.value_or(OpenEra::Graphics::GpuPreference{}));
    const auto result = OpenEra::App::RunTitleFolder(options, logger);
    if (result.Error != LaunchError::None)
    {
        return ExitCodeFor(result.Error);
    }
    return result.ExitCode ? std::bit_cast<int>(*result.ExitCode) : kExitSuccess;
}
} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): the program entry point.
int wmain(int argc, wchar_t** argv)
{
    try
    {
        const std::span<wchar_t*> raw(argv, argc);
        const std::vector<std::wstring> arguments(raw.begin() + 1, raw.end());
        if (!arguments.empty() && (arguments.front() == L"--help" || arguments.front() == L"-h"))
        {
            std::cout << kUsage;
            return kExitSuccess;
        }
        const auto line = ParseCommandLine(arguments);
        if (!line)
        {
            std::cerr << kUsage;
            return kExitUsage;
        }

        Config config(OpenEra::App::LauncherSettings());
        const auto loaded = LoadSettings(config, *line);
        if (line->Command == "config")
        {
            for (const auto& issue : loaded.Issues)
            {
                std::cerr << "openera: " << ToString(issue) << '\n';
            }
            return ShowSettings(config, loaded);
        }

        // The default logger, so shims that log through it (graphics) reach the same sinks.
        Logger& logger = OpenEra::Core::DefaultLogger();
        logger.SetMinimumLevel(LevelOf(config));
        const bool useWindow = config.GetBool(Setting::kLogWindow) && line->Command == "run";
        if (!useWindow)
        {
            logger.AddSink(std::make_shared<OpenEra::Core::StderrSink>());
        }
        if (const auto& file = config.GetString(Setting::kLogFile); !file.empty())
        {
            const auto mode = config.GetString(Setting::kLogFileMode) == "truncate" ? OpenEra::Core::FileMode::Truncate
                                                                                    : OpenEra::Core::FileMode::Append;
            logger.AddSink(std::make_shared<OpenEra::Core::FileSink>(
                std::filesystem::path(std::u8string(file.begin(), file.end())), mode));
        }
        if (config.GetBool(Setting::kLogDebugger))
        {
            logger.AddSink(std::make_shared<OpenEra::Core::DebuggerSink>());
        }
        std::shared_ptr<OpenEra::App::LogWindowSink> window;
        if (useWindow)
        {
            window = std::make_shared<OpenEra::App::LogWindowSink>(L"OpenEra log");
            logger.AddSink(window);
            DetachOwnConsole();
        }
        for (const auto& issue : loaded.Issues)
        {
            logger.Warn("Settings: {}", ToString(issue));
        }

        const int exitCode = line->Command == "info" ? ShowInfo(*line->Folder, logger) : Run(*line, config, logger);
        if (window && window->IsOpen())
        {
            logger.Info("openera: finished with exit code {}. Close the log window to exit.", exitCode);
            window->WaitUntilClosed();
        }
        logger.ClearSinks(); // The default logger outlives main; release the window now.
        return exitCode;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "openera: " << exception.what() << '\n';
        return kExitFailure;
    }
}
