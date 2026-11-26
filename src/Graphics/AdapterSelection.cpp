#include "AdapterSelection.h"

#include <atomic>
#include <bit>
#include <limits>

namespace OpenEra::Graphics
{
namespace
{
constexpr uint32_t kNoIndex = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kMaxIndex = 64;

std::atomic<GpuVendor>& PreferredVendor() noexcept
{
    static std::atomic<GpuVendor> vendor{GpuVendor::Auto};
    return vendor;
}

std::atomic<uint32_t>& PreferredIndex() noexcept
{
    static std::atomic<uint32_t> index{kNoIndex};
    return index;
}

constexpr std::optional<uint32_t> VendorIdOf(GpuVendor vendor) noexcept
{
    switch (vendor)
    {
    case GpuVendor::Nvidia:
        return kVendorNvidia;
    case GpuVendor::Amd:
        return kVendorAmd;
    case GpuVendor::Intel:
        return kVendorIntel;
    case GpuVendor::Auto:
        break;
    }
    return std::nullopt;
}

// Higher is better: software < other hardware < NVIDIA / AMD.
constexpr int Rank(const AdapterInfo& adapter) noexcept
{
    if (adapter.Software || adapter.VendorId == kVendorMicrosoft)
    {
        return 0;
    }
    return adapter.VendorId == kVendorNvidia || adapter.VendorId == kVendorAmd ? 2 : 1;
}

// Best adapter for which `eligible` is true; earlier adapters win ties.
template <typename Eligible>
std::optional<size_t> Best(std::span<const AdapterInfo> adapters, Eligible eligible) noexcept
{
    std::optional<size_t> best;
    int bestRank = 0;
    uint64_t bestMemory = 0;
    size_t index = 0;
    for (const AdapterInfo& candidate : adapters)
    {
        const int rank = Rank(candidate);
        if (eligible(candidate) &&
            (!best || rank > bestRank || (rank == bestRank && candidate.DedicatedVideoMemory > bestMemory)))
        {
            best = index;
            bestRank = rank;
            bestMemory = candidate.DedicatedVideoMemory;
        }
        ++index;
    }
    return best;
}
} // namespace

std::optional<size_t> SelectAdapter(std::span<const AdapterInfo> adapters, const GpuPreference& preference) noexcept
{
    if (preference.Index && *preference.Index < adapters.size())
    {
        return *preference.Index;
    }
    if (const auto vendorId = VendorIdOf(preference.Vendor))
    {
        const auto match = Best(adapters, [vendorId](const AdapterInfo& adapter) noexcept
                                { return !adapter.Software && adapter.VendorId == *vendorId; });
        if (match)
        {
            return match;
        }
    }
    return Best(adapters, [](const AdapterInfo&) noexcept { return true; });
}

std::optional<GpuPreference> ParseGpuPreference(std::string_view text) noexcept
{
    if (text == "auto")
    {
        return GpuPreference{};
    }
    if (text == "nvidia")
    {
        return GpuPreference{.Vendor = GpuVendor::Nvidia, .Index = std::nullopt};
    }
    if (text == "amd")
    {
        return GpuPreference{.Vendor = GpuVendor::Amd, .Index = std::nullopt};
    }
    if (text == "intel")
    {
        return GpuPreference{.Vendor = GpuVendor::Intel, .Index = std::nullopt};
    }
    if (text.empty() || text.size() > 2)
    {
        return std::nullopt;
    }
    uint32_t index = 0;
    for (const char digit : text)
    {
        if (digit < '0' || digit > '9')
        {
            return std::nullopt;
        }
        index = (index * 10) + (std::bit_cast<unsigned char>(digit) - uint32_t{'0'});
    }
    if (index > kMaxIndex)
    {
        return std::nullopt;
    }
    return GpuPreference{.Vendor = GpuVendor::Auto, .Index = index};
}

std::string_view VendorName(uint32_t vendorId) noexcept
{
    switch (vendorId)
    {
    case kVendorNvidia:
        return "NVIDIA";
    case kVendorAmd:
        return "AMD";
    case kVendorIntel:
        return "Intel";
    case kVendorMicrosoft:
        return "Microsoft";
    default:
        return "other";
    }
}

void SetGpuPreference(const GpuPreference& preference) noexcept
{
    PreferredVendor().store(preference.Vendor);
    PreferredIndex().store(preference.Index.value_or(kNoIndex));
}

GpuPreference CurrentGpuPreference() noexcept
{
    const uint32_t index = PreferredIndex().load();
    return {.Vendor = PreferredVendor().load(),
            .Index = index == kNoIndex ? std::nullopt : std::optional<uint32_t>(index)};
}
} // namespace OpenEra::Graphics
