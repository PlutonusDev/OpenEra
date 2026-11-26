#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenEra::Core
{
class Logger;
} // namespace OpenEra::Core

namespace OpenEra::Loader
{
// The parts of MicrosoftGame.config (the GDK title configuration, documented
// on Microsoft Learn) that OpenEra needs to start a title.
struct GameExecutable
{
    std::string Name; // Path relative to the title folder.
    std::string Id;
    std::string TargetDeviceFamily; // "Scarlett" (Xbox Series X|S), "XboxOne" or "PC".
};

struct GameConfig
{
    std::string IdentityName;
    std::string Publisher;
    std::string Version;
    std::string TitleId;
    std::string DisplayName;
    std::vector<GameExecutable> Executables;
};

enum class GameConfigError : uint8_t
{
    None,
    NotXml,
    NotGameConfig,
    NoExecutables,
    BadExecutable,
};

[[nodiscard]] std::string_view ToString(GameConfigError error) noexcept;

struct GameConfigResult
{
    GameConfigError Error = GameConfigError::None;
    std::optional<GameConfig> Config; // Set exactly when Error == GameConfigError::None.
};

// Elements OpenEra doesn't use are ignored; problems are logged.
[[nodiscard]] GameConfigResult ParseGameConfig(std::string_view text, Core::Logger& logger);

// The executable whose Id is `id`, or when `id` is empty, the first one for
// Xbox Series X|S, else the first one listed. Null if there's no match.
[[nodiscard]] const GameExecutable* SelectExecutable(const GameConfig& config, std::string_view id = {}) noexcept;
} // namespace OpenEra::Loader
