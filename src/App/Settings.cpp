#include "Settings.h"

#include <string>

#include <Windows.h>

namespace OpenEra::App
{
namespace
{
constexpr DWORD kMaxPath = 32'768;
} // namespace

std::vector<Core::SettingDefinition> LauncherSettings()
{
    using Core::SettingType;
    return {
        {.Key = std::string(Setting::kLogLevel),
         .Type = SettingType::String,
         .Default = std::string("info"),
         .Description = "Lowest level of log messages shown.",
         .Choices = {"trace", "info", "warn", "error"}},
        {.Key = std::string(Setting::kLogFile),
         .Type = SettingType::String,
         .Default = std::string(),
         .Description = "Also write the log to this file (empty: no file).",
         .Choices = {}},
        {.Key = std::string(Setting::kLogFileMode),
         .Type = SettingType::String,
         .Default = std::string("append"),
         .Description = "Whether the log file keeps earlier runs or starts afresh.",
         .Choices = {"append", "truncate"}},
        {.Key = std::string(Setting::kLogWindow),
         .Type = SettingType::Bool,
         .Default = false,
         .Description = "Show the log in its own window, which stays open after the title ends.",
         .Choices = {}},
        {.Key = std::string(Setting::kLogDebugger),
         .Type = SettingType::Bool,
         .Default = true,
         .Description = "Also send the log to an attached debugger's output window.",
         .Choices = {}},
        {.Key = std::string(Setting::kGpu),
         .Type = SettingType::String,
         .Default = std::string("auto"),
         .Description = "GPU for the title: auto (the NVIDIA or AMD GPU), nvidia, amd, intel or an adapter index.",
         .Choices = {}},
        {.Key = std::string(Setting::kOverlay),
         .Type = SettingType::String,
         .Default = std::string(),
         .Description = "Folder for the title's writes (empty: Overlay in the title's data folder).",
         .Choices = {}},
        {.Key = std::string(Setting::kExecutable),
         .Type = SettingType::String,
         .Default = std::string(),
         .Description = "Id of the executable to start (empty: the Xbox Series X|S one).",
         .Choices = {}},
    };
}

std::filesystem::path DataDirectory()
{
    std::wstring localAppData(kMaxPath, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), kMaxPath);
    localAppData.resize(length < kMaxPath ? length : 0);
    const std::filesystem::path base =
        localAppData.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(localAppData);
    return base / "OpenEra";
}

std::filesystem::path UserConfigPath()
{
    return DataDirectory() / "openera.toml";
}

std::filesystem::path TitleDataDirectory(std::string_view identity)
{
    std::string name;
    for (const char value : identity)
    {
        const bool safe = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                          (value >= '0' && value <= '9') || value == '.' || value == '-' || value == '_';
        name += safe ? value : '_';
    }
    if (name.empty() || name.find_first_not_of('.') == std::string::npos)
    {
        name = "Title";
    }
    return DataDirectory() / "Titles" / name;
}
} // namespace OpenEra::App
