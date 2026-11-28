#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace OpenEra::Kernel
{
class ShimRegistry;

// What the Gaming Runtime reports about the running title.
enum class DeviceType : uint32_t // NOLINT(performance-enum-size): ABI type.
{
    Unknown = 0x00,
    Pc = 0x01,
    XboxOne = 0x02,
    XboxScarlettAnaconda = 0x07, // Xbox Series X.
};

struct GameRuntimeTitle
{
    std::optional<uint32_t> TitleId; // From MicrosoftGame.config.
    DeviceType Device = DeviceType::Unknown;
};

// Set by the launcher for the title being run; the default reports no title id.
void SetGameRuntimeTitle(const GameRuntimeTitle& title) noexcept;
// From MicrosoftGame.config: a hex title id ("1A2B3C4D", optionally 0x-prefixed).
[[nodiscard]] std::optional<uint32_t> ParseTitleId(std::string_view text) noexcept;
// From an executable's TargetDeviceFamily: "Scarlett", "XboxOne" or "PC".
[[nodiscard]] DeviceType DeviceForFamily(std::string_view family) noexcept;

// xgameruntime.dll: XGameRuntimeInitialize / Uninitialize / IsFeatureAvailable,
// XGameGetXboxTitleId, XSystemGetDeviceType / GetConsoleId / GetXboxLiveSandboxId,
// and the XTaskQueue and XAsync families (Gdk::TaskQueue*, Gdk::Async*).
bool RegisterGameRuntimeShims(ShimRegistry& shims);
} // namespace OpenEra::Kernel
