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
#include "App/TitleLauncher.h"
#include "Core/Logger.h"
#include "Loader/GameConfig.h"

namespace
{
using OpenEra::App::LaunchError;
using OpenEra::Core::Logger;
using OpenEra::Core::LogLevel;

// Exit codes. After a title runs, openera exits with the title's own exit code.
constexpr int kExitSuccess = 0;
constexpr int kExitFailure = 1; // Unexpected error.
constexpr int kExitUsage = 2;
constexpr int kExitInput = 3;  // Not a folder, or a package.
constexpr int kExitConfig = 4; // MicrosoftGame.config missing or unusable, or no such executable.
constexpr int kExitLoad = 5;   // Loading or linking failed.
constexpr int kExitStart = 6;  // Start-up or the entry point failed.

constexpr std::string_view kUsage = R"(Usage:
  openera run <title folder> [options]   Load and start a title
  openera info <title folder>            Show what MicrosoftGame.config describes

The title folder is an unpacked title: the folder containing MicrosoftGame.config.

Options for run:
  --exe <Id>            Executable to start (default: the Xbox Series X|S one)
  --no-start            Load and link only
  --log-level <level>   trace, info (default), warn or error
  --log-file <path>     Also append the log to a file
  --log-window          Also show the log in a window; it stays open after the title ends
)";

struct CommandLine
{
    std::string Command;
    std::filesystem::path Folder;
    std::string ExecutableId;
    bool Start = true;
    LogLevel Level = LogLevel::Info;
    std::filesystem::path LogFile;
    bool LogWindow = false;
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

constexpr std::optional<LogLevel> ParseLevel(std::string_view text) noexcept
{
    if (text == "trace")
    {
        return LogLevel::Trace;
    }
    if (text == "info")
    {
        return LogLevel::Info;
    }
    if (text == "warn")
    {
        return LogLevel::Warn;
    }
    if (text == "error")
    {
        return LogLevel::Error;
    }
    return std::nullopt;
}

std::optional<CommandLine> ParseCommandLine(const std::vector<std::wstring>& arguments)
{
    if (arguments.size() < 2)
    {
        return std::nullopt;
    }
    CommandLine line;
    line.Command = Narrow(arguments.at(0));
    line.Folder = arguments.at(1);
    if (line.Command != "run" && line.Command != "info")
    {
        return std::nullopt;
    }
    for (size_t index = 2; index < arguments.size(); ++index)
    {
        const std::string option = Narrow(arguments.at(index));
        const bool hasValue = index + 1 < arguments.size();
        if (option == "--no-start")
        {
            line.Start = false;
        }
        else if (option == "--exe" && hasValue)
        {
            line.ExecutableId = Narrow(arguments.at(++index));
        }
        else if (option == "--log-window")
        {
            line.LogWindow = true;
        }
        else if (option == "--log-file" && hasValue)
        {
            line.LogFile = arguments.at(++index);
        }
        else if (option == "--log-level" && hasValue)
        {
            const auto level = ParseLevel(Narrow(arguments.at(++index)));
            if (!level)
            {
                return std::nullopt;
            }
            line.Level = *level;
        }
        else
        {
            return std::nullopt;
        }
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

int Run(const CommandLine& line, Logger& logger)
{
    OpenEra::App::LaunchOptions options;
    options.Folder = line.Folder;
    options.ExecutableId = line.ExecutableId;
    options.Start = line.Start;
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

        Logger logger;
        logger.SetMinimumLevel(line->Level);
        if (!line->LogWindow)
        {
            logger.AddSink(std::make_shared<OpenEra::Core::StderrSink>());
        }
        if (!line->LogFile.empty())
        {
            logger.AddSink(std::make_shared<OpenEra::Core::FileSink>(line->LogFile));
        }
        if (line->Command == "info")
        {
            return ShowInfo(line->Folder, logger);
        }
        std::shared_ptr<OpenEra::App::LogWindowSink> window;
        if (line->LogWindow)
        {
            window = std::make_shared<OpenEra::App::LogWindowSink>(L"OpenEra log");
            logger.AddSink(window);
            DetachOwnConsole();
        }
        const int exitCode = Run(*line, logger);
        if (window && window->IsOpen())
        {
            logger.Info("openera: finished with exit code {}. Close the log window to exit.", exitCode);
            window->WaitUntilClosed();
        }
        return exitCode;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "openera: " << exception.what() << '\n';
        return kExitFailure;
    }
}
