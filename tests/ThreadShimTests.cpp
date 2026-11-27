#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "Kernel/ThreadShims.h"

using namespace OpenEra::Kernel;

TEST_CASE("Affinity masks the host can satisfy are kept", "[threads]")
{
    CHECK(MapAffinityMask(0b0101, 0xFF) == 0b0101);
    CHECK(MapAffinityMask(0xFF, 0xFF) == 0xFF);
    CHECK(MapAffinityMask(0, 0xFF) == 0xFF); // Nothing asked: any processor.
    CHECK(MapAffinityMask(0b1, 0) == 0);     // Nothing available: leave it to the OS.
}

TEST_CASE("Console masks map onto hosts with fewer cores", "[threads]")
{
    // A 4-processor host, and a title pinning threads to console cores 0 to 6.
    constexpr uint64_t kHost = 0x0F;
    CHECK(MapAffinityMask(uint64_t{1} << 4, kHost) == 0b0001); // Core 4 wraps to processor 0.
    CHECK(MapAffinityMask(uint64_t{1} << 6, kHost) == 0b0100);
    CHECK(MapAffinityMask(0x7F, kHost) == kHost);
    CHECK(MapAffinityMask(0x30, kHost) == 0b0011);
}

TEST_CASE("Console masks map onto the available processors of larger hosts", "[threads]")
{
    // Processors 8 to 23 available to the process, say after a process affinity limit.
    constexpr uint64_t kHost = 0x00FF'FF00;
    CHECK(MapAffinityMask(0b1, kHost) == 0x100);
    CHECK(MapAffinityMask(0b11, kHost) == 0x300);
    CHECK(MapAffinityMask(uint64_t{1} << 63, kHost) == (uint64_t{1} << (8 + (63 % 16))));

    // A sparse host: only processors 1, 3 and 5.
    CHECK(MapAffinityMask(0b111, 0b10'1010) == 0b10'1010);
    CHECK(MapAffinityMask(uint64_t{1} << 3, 0b10'1010) == 0b1000); // Available: kept.
    CHECK(MapAffinityMask(uint64_t{1} << 4, 0b10'1010) == 0b1000); // 4 mod 3 = 1: the second one.
    CHECK(MapAffinityMask(uint64_t{1} << 6, 0b10'1010) == 0b10);   // 6 mod 3 = 0: the first one.
}

TEST_CASE("Thread priorities are clamped to documented levels", "[threads]")
{
    for (const int kept : {-15, -2, -1, 0, 1, 2, 15, kBackgroundBegin, kBackgroundEnd})
    {
        CHECK(MapThreadPriority(kept) == kept);
    }
    CHECK(MapThreadPriority(5) == kPriorityHighest);
    CHECK(MapThreadPriority(14) == kPriorityHighest);
    CHECK(MapThreadPriority(31) == kPriorityTimeCritical);
    CHECK(MapThreadPriority(-3) == kPriorityLowest);
    CHECK(MapThreadPriority(-14) == kPriorityLowest);
    CHECK(MapThreadPriority(-31) == kPriorityIdle);
}
