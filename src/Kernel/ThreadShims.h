#pragma once

#include <bit>
#include <cstdint>

namespace OpenEra::Kernel
{
class ShimModule;

// Microsoft Learn: SetThreadPriority.
inline constexpr int kPriorityIdle = -15;
inline constexpr int kPriorityLowest = -2;
inline constexpr int kPriorityHighest = 2;
inline constexpr int kPriorityTimeCritical = 15;
inline constexpr int kBackgroundBegin = 0x0001'0000;
inline constexpr int kBackgroundEnd = 0x0002'0000;

// An affinity mask a title asked for, made valid for the host: masks within
// `available` (the process affinity mask) are kept; otherwise bit i maps to
// the i-th available processor, wrapping when the host has fewer. Returns
// `available` if nothing maps (an empty request or no available processors).
[[nodiscard]] constexpr uint64_t MapAffinityMask(uint64_t requested, uint64_t available) noexcept
{
    if (available == 0 || requested == 0)
    {
        return available;
    }
    if ((requested & ~available) == 0)
    {
        return requested;
    }
    const int availableCount = std::popcount(available);
    uint64_t mapped = 0;
    for (int bit = 0; bit < 64; ++bit)
    {
        if ((requested & (uint64_t{1} << bit)) == 0)
        {
            continue;
        }
        // The (bit mod count)-th set bit of `available`, isolated.
        uint64_t remaining = available;
        for (int skip = bit % availableCount; skip > 0; --skip)
        {
            remaining &= remaining - 1;
        }
        mapped |= remaining & (~remaining + 1);
    }
    return mapped;
}

// A thread priority made valid for SetThreadPriority: the documented levels
// (THREAD_PRIORITY_IDLE, LOWEST .. HIGHEST, TIME_CRITICAL) and the background
// mode values are kept; anything else is clamped to the nearest level.
[[nodiscard]] constexpr int MapThreadPriority(int priority) noexcept
{
    if (priority == kPriorityIdle || priority == kPriorityTimeCritical || priority == kBackgroundBegin ||
        priority == kBackgroundEnd || (priority >= kPriorityLowest && priority <= kPriorityHighest))
    {
        return priority;
    }
    if (priority < kPriorityLowest)
    {
        return priority <= kPriorityIdle ? kPriorityIdle : kPriorityLowest;
    }
    return priority >= kPriorityTimeCritical ? kPriorityTimeCritical : kPriorityHighest;
}

// Adds SetThreadAffinityMask, SetThreadPriority and SetThreadDescription to a
// kernel32 shim module. Remapped masks and clamped priorities are reported
// (ReportPartialShim); thread names are also logged. Other thread and TLS
// functions behave as on desktop and pass through to the host.
bool AddThreadExports(ShimModule& kernel32);
} // namespace OpenEra::Kernel
