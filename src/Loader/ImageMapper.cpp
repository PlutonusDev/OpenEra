#include "ImageMapper.h"

#include <algorithm>
#include <array>
#include <bit>
#include <new>
#include <utility>

#include "Core/Logger.h"

namespace OpenEra::Loader
{
namespace
{
using Core::Logger;

constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept
{
    return (value + alignment - 1) / alignment * alignment;
}

// Section fields copied out of the packed header.
struct SectionLayout
{
    uint64_t VirtualAddress = 0;
    uint64_t VirtualSize = 0; // Falls back to SizeOfRawData when zero.
    uint64_t RawOffset = 0;
    uint64_t CopySize = 0; // File bytes that land in memory.
    uint32_t Characteristics = 0;
};

SectionLayout Layout(const SectionHeader& section) noexcept
{
    SectionLayout layout;
    layout.VirtualAddress = section.VirtualAddress;
    layout.VirtualSize = section.VirtualSize != 0 ? section.VirtualSize : section.SizeOfRawData;
    layout.RawOffset = section.PointerToRawData;
    layout.CopySize = std::min<uint64_t>(section.SizeOfRawData, layout.VirtualSize);
    layout.Characteristics = section.Characteristics;
    return layout;
}

MapError ValidateLayout(std::span<const std::byte> file, const PeImage& image, Logger& logger)
{
    const uint64_t sizeOfImage = image.OptionalHeader.SizeOfImage;
    const uint64_t sizeOfHeaders = image.OptionalHeader.SizeOfHeaders;
    if (sizeOfImage == 0 || sizeOfHeaders > sizeOfImage)
    {
        logger.Warn("PE: SizeOfHeaders {:#x} does not fit SizeOfImage {:#x}", sizeOfHeaders, sizeOfImage);
        return MapError::BadImageLayout;
    }

    uint64_t previousEnd = sizeOfHeaders;
    size_t index = 0;
    for (const auto& section : image.Sections)
    {
        const auto layout = Layout(section);
        const uint64_t end = layout.VirtualAddress + layout.VirtualSize;
        if (layout.VirtualAddress < previousEnd || end > sizeOfImage)
        {
            logger.Warn("PE: section {} [{:#x}, {:#x}) overlaps the headers or another section, or leaves the image",
                        index, layout.VirtualAddress, end);
            return MapError::BadImageLayout;
        }
        if (layout.RawOffset > file.size() || layout.CopySize > file.size() - layout.RawOffset)
        {
            logger.Warn("PE: section {} raw data [{:#x}, +{:#x}) runs past the end of the file", index,
                        layout.RawOffset, layout.CopySize);
            return MapError::TruncatedSection;
        }
        previousEnd = end;
        ++index;
    }
    return MapError::None;
}

void CopyImage(std::span<const std::byte> file, const PeImage& image, std::span<std::byte> memory) noexcept
{
    const size_t headerBytes = std::min<size_t>(image.OptionalHeader.SizeOfHeaders, file.size());
    std::ranges::copy(file.first(headerBytes), memory.begin());

    for (const auto& section : image.Sections)
    {
        const auto layout = Layout(section);
        std::ranges::copy(file.subspan(layout.RawOffset, layout.CopySize),
                          memory.subspan(layout.VirtualAddress).begin());
    }
}

MapError ApplyRelocations(const PeImage& image, std::span<std::byte> memory, uint64_t delta, Logger& logger)
{
    for (const uint32_t rva : image.RelocationRvas)
    {
        if (rva > memory.size() || memory.size() - rva < sizeof(uint64_t))
        {
            logger.Warn("PE: relocation at RVA {:#x} is outside the mapped image", rva);
            return MapError::BadRelocation;
        }

        const auto field = memory.subspan(rva, sizeof(uint64_t));
        std::array<std::byte, sizeof(uint64_t)> raw{};
        std::ranges::copy(field, raw.begin());
        raw = std::bit_cast<std::array<std::byte, sizeof(uint64_t)>>(std::bit_cast<uint64_t>(raw) + delta);
        std::ranges::copy(raw, field.begin());
    }
    return MapError::None;
}

MapError ApplyProtections(const PeImage& image, ImageMemory& memory, Logger& logger)
{
    const uint64_t sizeOfImage = image.OptionalHeader.SizeOfImage;
    const uint64_t alignment = image.OptionalHeader.SectionAlignment;
    if (alignment < kPageSize)
    {
        // Sections share pages, so they can't get separate protections.
        logger.Warn("PE: section alignment {:#x} is below the page size; image mapped read-write-execute", alignment);
        if (!memory.Protect(0, sizeOfImage, PageProtection::ExecuteReadWrite))
        {
            logger.Warn("PE: failed to set image protection");
            return MapError::ProtectionFailed;
        }
        return MapError::None;
    }

    const uint64_t headersEnd = std::min(AlignUp(image.OptionalHeader.SizeOfHeaders, kPageSize), sizeOfImage);
    if (!memory.Protect(0, headersEnd, PageProtection::ReadOnly))
    {
        logger.Warn("PE: failed to protect the headers");
        return MapError::ProtectionFailed;
    }

    size_t index = 0;
    for (const auto& section : image.Sections)
    {
        const auto layout = Layout(section);
        const uint64_t length = std::min(AlignUp(layout.VirtualSize, alignment), sizeOfImage - layout.VirtualAddress);
        const auto protection = SectionProtection(layout.Characteristics);
        if (protection == PageProtection::ExecuteReadWrite)
        {
            logger.Warn("PE: section {} is writable and executable", index);
        }
        if (length != 0 && !memory.Protect(layout.VirtualAddress, length, protection))
        {
            logger.Warn("PE: failed to set section {} to {}", index, ToString(protection));
            return MapError::ProtectionFailed;
        }
        ++index;
    }
    return MapError::None;
}

MapResult MapImageImpl(std::span<const std::byte> file, const PeImage& image, ImageAllocator& allocator, Logger& logger)
{
    if (const auto error = ValidateLayout(file, image, logger); error != MapError::None)
    {
        return {.Error = error, .Image = std::nullopt};
    }

    const uint64_t preferredBase = image.OptionalHeader.ImageBase;
    const uint32_t sizeOfImage = image.OptionalHeader.SizeOfImage;
    auto memory = allocator.Allocate(preferredBase, sizeOfImage);
    if (!memory || memory->Bytes().size() < sizeOfImage)
    {
        logger.Warn("PE: could not allocate {:#x} bytes for the image", sizeOfImage);
        return {.Error = MapError::AllocationFailed, .Image = std::nullopt};
    }

    CopyImage(file, image, memory->Bytes());

    const uint64_t base = memory->Base();
    if (base != preferredBase)
    {
        if ((image.FileHeader.Characteristics & kFileRelocationsStripped) != 0)
        {
            logger.Warn("PE: image has no relocations and can't load at {:#x} instead of {:#x}", base, preferredBase);
            return {.Error = MapError::RelocationsStripped, .Image = std::nullopt};
        }
        if (const auto error = ApplyRelocations(image, memory->Bytes(), base - preferredBase, logger);
            error != MapError::None)
        {
            return {.Error = error, .Image = std::nullopt};
        }
    }

    if (const auto error = ApplyProtections(image, *memory, logger); error != MapError::None)
    {
        return {.Error = error, .Image = std::nullopt};
    }

    logger.Trace("PE: mapped {:#x} bytes at {:#x} (preferred {:#x})", sizeOfImage, base, preferredBase);
    return {.Error = MapError::None, .Image = MappedImage{.Memory = std::move(memory), .PreferredBase = preferredBase}};
}
} // namespace

PageProtection SectionProtection(uint32_t characteristics) noexcept
{
    const bool execute = (characteristics & kSectionMemoryExecute) != 0;
    const bool read = (characteristics & kSectionMemoryRead) != 0;
    const bool write = (characteristics & kSectionMemoryWrite) != 0;
    if (execute)
    {
        if (write)
        {
            return PageProtection::ExecuteReadWrite;
        }
        return read ? PageProtection::ExecuteRead : PageProtection::Execute;
    }
    if (write)
    {
        return PageProtection::ReadWrite; // Windows has no write-only pages.
    }
    return read ? PageProtection::ReadOnly : PageProtection::NoAccess;
}

std::string_view ToString(MapError error) noexcept
{
    switch (error)
    {
    case MapError::None:
        return "None";
    case MapError::BadImageLayout:
        return "BadImageLayout";
    case MapError::TruncatedSection:
        return "TruncatedSection";
    case MapError::AllocationFailed:
        return "AllocationFailed";
    case MapError::RelocationsStripped:
        return "RelocationsStripped";
    case MapError::BadRelocation:
        return "BadRelocation";
    case MapError::ProtectionFailed:
        return "ProtectionFailed";
    case MapError::OutOfMemory:
        return "OutOfMemory";
    }
    return "Unknown";
}

MapResult MapImage(std::span<const std::byte> file, const PeImage& image, ImageAllocator& allocator) noexcept
{
    return MapImage(file, image, allocator, Core::DefaultLogger());
}

MapResult MapImage(std::span<const std::byte> file, const PeImage& image, ImageAllocator& allocator,
                   Core::Logger& logger) noexcept
{
    try
    {
        return MapImageImpl(file, image, allocator, logger);
    }
    catch (const std::bad_alloc&)
    {
        return {.Error = MapError::OutOfMemory, .Image = std::nullopt};
    }
}
} // namespace OpenEra::Loader
