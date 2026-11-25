#include "ImageMemory.h"

#include <bit>
#include <new>
#include <utility>

#include <Windows.h>

namespace OpenEra::Loader
{
namespace
{
DWORD ToWin32Protection(PageProtection protection) noexcept
{
    switch (protection)
    {
    case PageProtection::NoAccess:
        return PAGE_NOACCESS;
    case PageProtection::ReadOnly:
        return PAGE_READONLY;
    case PageProtection::ReadWrite:
        return PAGE_READWRITE;
    case PageProtection::Execute:
        return PAGE_EXECUTE;
    case PageProtection::ExecuteRead:
        return PAGE_EXECUTE_READ;
    case PageProtection::ExecuteReadWrite:
        return PAGE_EXECUTE_READWRITE;
    }
    return PAGE_NOACCESS;
}

struct VirtualFreeDeleter
{
    void operator()(void* address) const noexcept { VirtualFree(address, 0, MEM_RELEASE); }
};

using VirtualRegion = std::unique_ptr<void, VirtualFreeDeleter>;

class VirtualImageMemory final : public ImageMemory
{
public:
    VirtualImageMemory(VirtualRegion region, size_t size) noexcept
        : m_region(std::move(region))
        , m_size(size)
    {
    }

    [[nodiscard]] uint64_t Base() const noexcept override { return std::bit_cast<uint64_t>(m_region.get()); }

    [[nodiscard]] std::span<std::byte> Bytes() noexcept override
    {
        return {static_cast<std::byte*>(m_region.get()), m_size};
    }

    [[nodiscard]] bool Protect(size_t offset, size_t size, PageProtection protection) noexcept override
    {
        if (offset > m_size || size > m_size - offset)
        {
            return false;
        }
        DWORD previous = 0;
        return VirtualProtect(Bytes().subspan(offset, size).data(), size, ToWin32Protection(protection), &previous) !=
               FALSE;
    }

private:
    VirtualRegion m_region;
    size_t m_size;
};

class VirtualImageAllocator final : public ImageAllocator
{
public:
    [[nodiscard]] std::unique_ptr<ImageMemory> Allocate(uint64_t preferredBase, size_t size) noexcept override
    {
        if (size == 0)
        {
            return nullptr;
        }

        constexpr DWORD kAllocationType = MEM_RESERVE | MEM_COMMIT;
        VirtualRegion region(VirtualAlloc(std::bit_cast<void*>(preferredBase), size, kAllocationType, PAGE_READWRITE));
        if (!region)
        {
            region.reset(VirtualAlloc(nullptr, size, kAllocationType, PAGE_READWRITE));
        }
        if (!region)
        {
            return nullptr;
        }

        try
        {
            return std::make_unique<VirtualImageMemory>(std::move(region), size);
        }
        catch (const std::bad_alloc&)
        {
            return nullptr; // `region` still owns the allocation and releases it.
        }
    }
};
} // namespace

std::string_view ToString(PageProtection protection) noexcept
{
    switch (protection)
    {
    case PageProtection::NoAccess:
        return "NoAccess";
    case PageProtection::ReadOnly:
        return "ReadOnly";
    case PageProtection::ReadWrite:
        return "ReadWrite";
    case PageProtection::Execute:
        return "Execute";
    case PageProtection::ExecuteRead:
        return "ExecuteRead";
    case PageProtection::ExecuteReadWrite:
        return "ExecuteReadWrite";
    }
    return "Unknown";
}

std::unique_ptr<ImageAllocator> CreateVirtualImageAllocator()
{
    return std::make_unique<VirtualImageAllocator>();
}
} // namespace OpenEra::Loader
