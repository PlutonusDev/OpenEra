#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "Core/Config.h"

namespace OpenEra::App
{
// Setting keys the launcher reads.
namespace Setting
{
inline constexpr std::string_view kLogLevel = "log.level";
inline constexpr std::string_view kLogFile = "log.file";
inline constexpr std::string_view kLogFileMode = "log.file_mode";
inline constexpr std::string_view kLogWindow = "log.window";
inline constexpr std::string_view kLogDebugger = "log.debugger";
inline constexpr std::string_view kGpu = "graphics.gpu";
inline constexpr std::string_view kOverlay = "files.overlay";
inline constexpr std::string_view kExecutable = "title.executable";
} // namespace Setting

// Every setting openera knows, with defaults and descriptions.
[[nodiscard]] std::vector<Core::SettingDefinition> LauncherSettings();

// %LOCALAPPDATA%\OpenEra (the temp directory if that isn't set).
[[nodiscard]] std::filesystem::path DataDirectory();
// The user's settings: <DataDirectory>\openera.toml.
[[nodiscard]] std::filesystem::path UserConfigPath();
// Per-title data, named after the title's identity: <DataDirectory>\Titles\<identity>.
// Its openera.toml overrides the user's settings for that title, and its
// Overlay folder holds the title's writes.
[[nodiscard]] std::filesystem::path TitleDataDirectory(std::string_view identity);
} // namespace OpenEra::App
