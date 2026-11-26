#pragma once

#include <cstdint>

namespace OpenEra::Kernel
{
class ShimModule;

// Documented VirtualAlloc allocation types and protections (Microsoft Learn:
// VirtualAlloc, Memory Protection Constants).
inline constexpr uint32_t kKnownAllocationTypes = 0x0000'1000 /*MEM_COMMIT*/ | 0x0000'2000 /*MEM_RESERVE*/ |
                                                  0x0008'0000 /*MEM_RESET*/ | 0x0100'0000 /*MEM_RESET_UNDO*/ |
                                                  0x0010'0000 /*MEM_TOP_DOWN*/ | 0x0020'0000 /*MEM_WRITE_WATCH*/ |
                                                  0x0040'0000 /*MEM_PHYSICAL*/ | 0x2000'0000 /*MEM_LARGE_PAGES*/;
inline constexpr uint32_t kKnownProtections = 0x0000'00FF /*PAGE_NOACCESS ... PAGE_EXECUTE_WRITECOPY*/ |
                                              0x0000'0100 /*PAGE_GUARD*/ | 0x0000'0200 /*PAGE_NOCACHE*/ |
                                              0x0000'0400 /*PAGE_WRITECOMBINE*/ | 0x4000'0000 /*PAGE_TARGETS_INVALID*/;

// VirtualAlloc flags split into what desktop Windows documents and the rest.
struct VirtualAllocFlags
{
    uint32_t AllocationType = 0; // Documented MEM_* bits only.
    uint32_t Protection = 0;     // Documented PAGE_* value and modifiers only.
    uint32_t UnknownAllocationType = 0;
    uint32_t UnknownProtection = 0;
};

[[nodiscard]] constexpr VirtualAllocFlags SanitizeVirtualAllocFlags(uint32_t allocationType,
                                                                    uint32_t protection) noexcept
{
    return {.AllocationType = allocationType & kKnownAllocationTypes,
            .Protection = protection & kKnownProtections,
            .UnknownAllocationType = allocationType & ~kKnownAllocationTypes,
            .UnknownProtection = protection & ~kKnownProtections};
}

// Adds VirtualAlloc to a kernel32 shim module. Undocumented allocation-type and
// protection bits are reported (ReportPartialShim) and dropped, and a large-page
// request falls back to normal pages when the process lacks the lock-pages
// privilege, which console titles never have to ask for. Other memory functions
// behave as on desktop and pass through to the host.
bool AddMemoryExports(ShimModule& kernel32);
} // namespace OpenEra::Kernel
