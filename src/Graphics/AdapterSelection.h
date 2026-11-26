#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace OpenEra::Graphics
{
// PCI vendor IDs as reported in DXGI_ADAPTER_DESC1::VendorId.
inline constexpr uint32_t kVendorNvidia = 0x10DE;
inline constexpr uint32_t kVendorAmd = 0x1002;
inline constexpr uint32_t kVendorIntel = 0x8086;
inline constexpr uint32_t kVendorMicrosoft = 0x1414; // WARP / Basic Render Driver.

struct AdapterInfo
{
    std::string Description;
    uint32_t VendorId = 0;
    uint32_t DeviceId = 0;
    uint64_t Luid = 0;
    uint64_t DedicatedVideoMemory = 0;
    bool Software = false;
};

enum class GpuVendor : uint8_t
{
    Auto, // NVIDIA or AMD first, then any other hardware GPU, then software.
    Nvidia,
    Amd,
    Intel,
};

struct GpuPreference
{
    GpuVendor Vendor = GpuVendor::Auto;
    std::optional<uint32_t> Index; // Position in DXGI's own adapter order; overrides Vendor.
};

// Index into `adapters` (in DXGI's order) of the GPU titles should use, or
// nullopt if there are none. A preference that matches nothing falls back to Auto.
[[nodiscard]] std::optional<size_t> SelectAdapter(std::span<const AdapterInfo> adapters,
                                                  const GpuPreference& preference) noexcept;

// "auto", "nvidia", "amd", "intel" or an adapter index.
[[nodiscard]] std::optional<GpuPreference> ParseGpuPreference(std::string_view text) noexcept;

[[nodiscard]] std::string_view VendorName(uint32_t vendorId) noexcept;

// Process-wide preference the graphics shims use. Defaults to Auto.
void SetGpuPreference(const GpuPreference& preference) noexcept;
[[nodiscard]] GpuPreference CurrentGpuPreference() noexcept;
} // namespace OpenEra::Graphics
