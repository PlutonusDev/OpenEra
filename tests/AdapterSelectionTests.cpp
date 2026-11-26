#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Graphics/AdapterSelection.h"

using namespace OpenEra::Graphics;

namespace
{
constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;

AdapterInfo Gpu(uint32_t vendor, uint64_t memory, bool software = false)
{
    return {.Description = std::string(VendorName(vendor)),
            .VendorId = vendor,
            .DeviceId = 1,
            .Luid = memory + vendor,
            .DedicatedVideoMemory = memory,
            .Software = software};
}

AdapterInfo Integrated()
{
    return Gpu(kVendorIntel, kGiB / 8);
}

AdapterInfo Nvidia()
{
    return Gpu(kVendorNvidia, 8 * kGiB);
}

AdapterInfo Amd()
{
    return Gpu(kVendorAmd, 16 * kGiB);
}

AdapterInfo Warp()
{
    return Gpu(kVendorMicrosoft, 0, true);
}
} // namespace

TEST_CASE("Hybrid laptops get the NVIDIA or AMD GPU, not the integrated one", "[graphics]")
{
    const std::vector nvidiaLaptop = {Integrated(), Nvidia(), Warp()};
    CHECK(SelectAdapter(nvidiaLaptop, {}) == std::optional<size_t>(1));

    const std::vector amdLaptop = {Integrated(), Amd(), Warp()};
    CHECK(SelectAdapter(amdLaptop, {}) == std::optional<size_t>(1));
}

TEST_CASE("Among NVIDIA and AMD GPUs the one with more dedicated memory wins", "[graphics]")
{
    const std::vector both = {Nvidia(), Amd()};
    CHECK(SelectAdapter(both, {}) == std::optional<size_t>(1));

    const std::vector twins = {Nvidia(), Nvidia()};
    CHECK(SelectAdapter(twins, {}) == std::optional<size_t>(0)); // Ties keep DXGI's order.
}

TEST_CASE("Without NVIDIA or AMD, other hardware beats software", "[graphics]")
{
    const std::vector intelOnly = {Warp(), Integrated()};
    CHECK(SelectAdapter(intelOnly, {}) == std::optional<size_t>(1));

    const std::vector softwareOnly = {Warp()};
    CHECK(SelectAdapter(softwareOnly, {}) == std::optional<size_t>(0));

    CHECK_FALSE(SelectAdapter({}, {}).has_value());
}

TEST_CASE("A vendor or index preference overrides the default", "[graphics]")
{
    const std::vector all = {Integrated(), Nvidia(), Amd(), Warp()};
    CHECK(SelectAdapter(all, {.Vendor = GpuVendor::Nvidia, .Index = std::nullopt}) == std::optional<size_t>(1));
    CHECK(SelectAdapter(all, {.Vendor = GpuVendor::Amd, .Index = std::nullopt}) == std::optional<size_t>(2));
    CHECK(SelectAdapter(all, {.Vendor = GpuVendor::Intel, .Index = std::nullopt}) == std::optional<size_t>(0));
    CHECK(SelectAdapter(all, {.Vendor = GpuVendor::Auto, .Index = 3}) == std::optional<size_t>(3));

    // Nothing matches: fall back to the default choice.
    const std::vector noAmd = {Integrated(), Nvidia()};
    CHECK(SelectAdapter(noAmd, {.Vendor = GpuVendor::Amd, .Index = std::nullopt}) == std::optional<size_t>(1));
    CHECK(SelectAdapter(noAmd, {.Vendor = GpuVendor::Auto, .Index = 9}) == std::optional<size_t>(1));
}

TEST_CASE("GPU preferences parse from the command line", "[graphics]")
{
    CHECK(ParseGpuPreference("auto").value_or(GpuPreference{.Vendor = GpuVendor::Intel, .Index = 1}).Vendor ==
          GpuVendor::Auto);
    CHECK(ParseGpuPreference("nvidia").value_or(GpuPreference{}).Vendor == GpuVendor::Nvidia);
    CHECK(ParseGpuPreference("amd").value_or(GpuPreference{}).Vendor == GpuVendor::Amd);
    CHECK(ParseGpuPreference("intel").value_or(GpuPreference{}).Vendor == GpuVendor::Intel);
    CHECK(ParseGpuPreference("2").value_or(GpuPreference{}).Index == std::optional<uint32_t>(2));
    CHECK_FALSE(ParseGpuPreference("").has_value());
    CHECK_FALSE(ParseGpuPreference("geforce").has_value());
    CHECK_FALSE(ParseGpuPreference("-1").has_value());
    CHECK_FALSE(ParseGpuPreference("999").has_value());
}

TEST_CASE("The process-wide GPU preference round-trips", "[graphics]")
{
    SetGpuPreference({.Vendor = GpuVendor::Amd, .Index = 4});
    CHECK(CurrentGpuPreference().Vendor == GpuVendor::Amd);
    CHECK(CurrentGpuPreference().Index == std::optional<uint32_t>(4));
    SetGpuPreference({});
    CHECK(CurrentGpuPreference().Vendor == GpuVendor::Auto);
    CHECK_FALSE(CurrentGpuPreference().Index.has_value());
}
