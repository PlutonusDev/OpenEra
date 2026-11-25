#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace OpenEra::Loader
{
enum class PageProtection : uint8_t
{
    NoAccess,
    ReadOnly,
    ReadWrite,
    Execute,
    ExecuteRead,
    ExecuteReadWrite,
};

[[nodiscard]] std::string_view ToString(PageProtection protection) noexcept;

// A committed region an image is mapped into. Releases the region when destroyed.
class ImageMemory
{
public:
    ImageMemory() = default;
    virtual ~ImageMemory() = default;

    ImageMemory(const ImageMemory&) = delete;
    ImageMemory& operator=(const ImageMemory&) = delete;
    ImageMemory(ImageMemory&&) = delete;
    ImageMemory& operator=(ImageMemory&&) = delete;

    // Address the image sees itself at (relocations are relative to this).
    [[nodiscard]] virtual uint64_t Base() const noexcept = 0;
    [[nodiscard]] virtual std::span<std::byte> Bytes() noexcept = 0;
    [[nodiscard]] virtual bool Protect(size_t offset, size_t size, PageProtection protection) noexcept = 0;
};

class ImageAllocator
{
public:
    ImageAllocator() = default;
    virtual ~ImageAllocator() = default;

    ImageAllocator(const ImageAllocator&) = delete;
    ImageAllocator& operator=(const ImageAllocator&) = delete;
    ImageAllocator(ImageAllocator&&) = delete;
    ImageAllocator& operator=(ImageAllocator&&) = delete;

    // Returns zero-filled, read-write memory, at `preferredBase` when possible.
    // Returns null on failure.
    [[nodiscard]] virtual std::unique_ptr<ImageMemory> Allocate(uint64_t preferredBase, size_t size) noexcept = 0;
};

// Allocator backed by VirtualAlloc / VirtualProtect / VirtualFree.
[[nodiscard]] std::unique_ptr<ImageAllocator> CreateVirtualImageAllocator();
} // namespace OpenEra::Loader
