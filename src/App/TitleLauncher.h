#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "Loader/GameConfig.h"

namespace OpenEra::Core
{
class Logger;
} // namespace OpenEra::Core

namespace OpenEra::App
{
inline constexpr std::string_view kConfigFileName = "MicrosoftGame.config";

enum class LaunchError : uint8_t
{
    None,
    NotAFolder,
    PackagedInput, // An .xvc / .msixvc package: OpenEra never decrypts or unpacks them.
    NoConfig,
    BadConfig,
    NoExecutable,
    BadExecutablePath,
    LoadFailed,
    StartFailed,
};

[[nodiscard]] std::string_view ToString(LaunchError error) noexcept;

struct TitleConfigResult
{
    LaunchError Error = LaunchError::None;
    std::optional<Loader::GameConfig> Config; // Set exactly when Error == LaunchError::None.
};

// Reads MicrosoftGame.config from an unpacked title folder.
[[nodiscard]] TitleConfigResult ReadTitleConfig(const std::filesystem::path& folder, Core::Logger& logger);

struct LaunchOptions
{
    std::filesystem::path Folder;
    std::string ExecutableId;            // Empty: Loader::SelectExecutable's choice.
    bool Start = true;                   // False: map and link only.
    std::filesystem::path TlsSlot;       // Empty: next to the running executable.
    std::filesystem::path WorkDirectory; // Empty: OpenEra under the temp directory.
    // Where the title's writes go (the folder itself is never modified).
    // Empty: Overlay in App::TitleDataDirectory(identity).
    std::filesystem::path OverlayDirectory;
};

struct LaunchResult
{
    LaunchError Error = LaunchError::None;
    std::string Executable;           // Name from the config.
    std::optional<uint32_t> ExitCode; // Set when the entry point ran and returned.
};

// Loads a title folder's main executable with its own DLLs, links it, and
// (with Start) runs its start-up and entry point. Logs the unimplemented-call
// summary at the end.
[[nodiscard]] LaunchResult RunTitleFolder(const LaunchOptions& options, Core::Logger& logger);
} // namespace OpenEra::App
