#include "ThreadShims.h"

#include <bit>
#include <format>
#include <string>

#include <Windows.h>

#include "Core/Logger.h"
#include "Kernel/ShimGuard.h"
#include "Kernel/ShimModule.h"

namespace OpenEra::Kernel
{
namespace
{
constexpr DWORD kMaxThreadName = 4'096;

std::string Narrow(PCWSTR text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
    {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    result.pop_back(); // Terminator.
    return result;
}

DWORD_PTR WINAPI SetThreadAffinityMaskShim(HANDLE thread, DWORD_PTR mask) noexcept
{
    return GuardShim<DWORD_PTR>("SetThreadAffinityMask", 0, 0,
                                [=]() -> DWORD_PTR
                                {
                                    DWORD_PTR process = 0;
                                    DWORD_PTR system = 0;
                                    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) == FALSE)
                                    {
                                        return SetThreadAffinityMask(thread, mask);
                                    }
                                    const uint64_t mapped = MapAffinityMask(mask, process);
                                    if (mapped != mask)
                                    {
                                        ReportPartialShim(StubKind::Win32Import, "kernel32", "SetThreadAffinityMask",
                                                          std::format("mask {:#x} mapped to {:#x}", mask, mapped));
                                    }
                                    return SetThreadAffinityMask(thread, mapped);
                                });
}

BOOL WINAPI SetThreadPriorityShim(HANDLE thread, int priority) noexcept
{
    return GuardShim<BOOL>("SetThreadPriority", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               const int mapped = MapThreadPriority(priority);
                               if (mapped != priority)
                               {
                                   ReportPartialShim(StubKind::Win32Import, "kernel32", "SetThreadPriority",
                                                     std::format("priority {} clamped to {}", priority, mapped));
                               }
                               return SetThreadPriority(thread, mapped);
                           });
}

HRESULT WINAPI SetThreadDescriptionShim(HANDLE thread, PCWSTR description) noexcept
{
    return std::bit_cast<HRESULT>(GuardHResult(
        "SetThreadDescription",
        [=]() -> HResult
        {
            if (description != nullptr && wcsnlen(description, kMaxThreadName) < kMaxThreadName)
            {
                Core::LogTrace("Threads: thread {} named \"{}\"", GetThreadId(thread), Narrow(description));
            }
            return std::bit_cast<HResult>(SetThreadDescription(thread, description));
        }));
}
} // namespace

bool AddThreadExports(ShimModule& kernel32)
{
    return kernel32.Export("SetThreadAffinityMask", ToShimProc(&SetThreadAffinityMaskShim)) &&
           kernel32.Export("SetThreadPriority", ToShimProc(&SetThreadPriorityShim)) &&
           kernel32.Export("SetThreadDescription", ToShimProc(&SetThreadDescriptionShim));
}
} // namespace OpenEra::Kernel
