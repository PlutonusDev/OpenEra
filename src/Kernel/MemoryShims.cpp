#include "MemoryShims.h"

#include <atomic>
#include <bit>
#include <cstdint>
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
using VirtualAlloc2Function = PVOID WINAPI(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

// VirtualAlloc2 (Windows 10 1803+), looked up so older systems still load.
VirtualAlloc2Function* FindVirtualAlloc2() noexcept
{
    const HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    auto* const proc = kernelBase == nullptr ? nullptr : GetProcAddress(kernelBase, "VirtualAlloc2");
    return std::bit_cast<VirtualAlloc2Function*>(std::bit_cast<uintptr_t>(proc));
}

// Normal pages in place of large pages, keeping the large-page alignment
// titles rely on when they let the system pick the address.
// MEM_EXTENDED_PARAMETER is a union by its API definition.
[[gsl::suppress("type.7")]] LPVOID AllocateLargePageFallback(LPVOID address, SIZE_T size, DWORD allocationType,
                                                             DWORD protection) noexcept
{
    static VirtualAlloc2Function* const kVirtualAlloc2 = FindVirtualAlloc2();
    const SIZE_T alignment = GetLargePageMinimum();
    if (address != nullptr || kVirtualAlloc2 == nullptr || alignment == 0)
    {
        return VirtualAlloc(address, size, allocationType, protection);
    }
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.Alignment = alignment;
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    return kVirtualAlloc2(GetCurrentProcess(), nullptr, size, allocationType, protection, &parameter, 1);
}

LPVOID WINAPI VirtualAllocShim(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protection) noexcept
{
    return GuardShim<LPVOID>(
        "VirtualAlloc", nullptr, nullptr,
        [=]() -> LPVOID
        {
            const auto flags = SanitizeVirtualAllocFlags(allocationType, protection);
            if (flags.UnknownAllocationType != 0 || flags.UnknownProtection != 0)
            {
                ReportPartialShim(StubKind::Win32Import, "kernel32", "VirtualAlloc",
                                  std::format("dropped allocation type {:#x}, protection {:#x}",
                                              flags.UnknownAllocationType, flags.UnknownProtection));
            }
            LPVOID result = VirtualAlloc(address, size, flags.AllocationType, flags.Protection);
            constexpr uint32_t kLargePages = MEM_LARGE_PAGES;
            if (result == nullptr && (flags.AllocationType & kLargePages) != 0 &&
                GetLastError() == ERROR_PRIVILEGE_NOT_HELD)
            {
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true))
                {
                    Core::LogWarn("Memory: no lock-pages privilege, so large-page allocations use normal pages");
                }
                result =
                    AllocateLargePageFallback(address, size, flags.AllocationType & ~kLargePages, flags.Protection);
            }
            return result;
        });
}
} // namespace

bool AddMemoryExports(ShimModule& kernel32)
{
    return kernel32.Export("VirtualAlloc", ToShimProc(&VirtualAllocShim));
}
} // namespace OpenEra::Kernel
