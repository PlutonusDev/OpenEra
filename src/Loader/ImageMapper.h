#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "Loader/ImageMemory.h"
#include "Loader/PeReader.h"

namespace OpenEra::Core
{
class Logger;
} // namespace OpenEra::Core

namespace OpenEra::Loader
{
// Section and file header flags from the PE Format specification.
constexpr uint32_t kSectionMemoryExecute = 0x2000'0000;
constexpr uint32_t kSectionMemoryRead = 0x4000'0000;
constexpr uint32_t kSectionMemoryWrite = 0x8000'0000;
constexpr uint16_t kFileRelocationsStripped = 0x0001;
constexpr uint32_t kPageSize = 0x1000;

[[nodiscard]] PageProtection SectionProtection(uint32_t characteristics) noexcept;

enum class MapError : uint8_t
{
    None,
    BadImageLayout,
    TruncatedSection,
    AllocationFailed,
    RelocationsStripped,
    BadRelocation,
    ProtectionFailed,
    OutOfMemory,
};

[[nodiscard]] std::string_view ToString(MapError error) noexcept;

struct MappedImage
{
    std::unique_ptr<ImageMemory> Memory;
    uint64_t PreferredBase = 0;

    [[nodiscard]] uint64_t Base() const noexcept { return Memory ? Memory->Base() : 0; }
    [[nodiscard]] bool Relocated() const noexcept { return Base() != PreferredBase; }
};

struct MapResult
{
    MapError Error = MapError::None;
    std::optional<MappedImage> Image; // Set exactly when Error == MapError::None.
};

// Lays out a parsed PE32+ image in memory: headers and section data copied to
// their RVAs (tails zero-filled), base relocations applied if it isn't at its
// preferred base, then final page protections set. `file` must be the bytes
// `image` was parsed from.
[[nodiscard]] MapResult MapImage(std::span<const std::byte> file, const PeImage& image,
                                 ImageAllocator& allocator) noexcept;
[[nodiscard]] MapResult MapImage(std::span<const std::byte> file, const PeImage& image, ImageAllocator& allocator,
                                 Core::Logger& logger) noexcept;
} // namespace OpenEra::Loader
