#pragma once

// XVD/XVC container layout. Field offsets, sizes and constants are facts
// documented by xvdtool (https://github.com/emoose/xvdtool, GPL-2.0);
// no code was taken from it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace OpenEra::Core
{
class Logger;
} // namespace OpenEra::Core

namespace OpenEra::Loader
{
constexpr uint64_t kXvdMagic = 0x6476'782D'7466'736D; // "msft-xvd"
constexpr size_t kXvdSignatureSize = 0x200;
constexpr uint64_t kXvdHeaderRegionSize = 0x3000;
constexpr uint64_t kXvdPageSize = 0x1000;
constexpr uint32_t kXvdBlockSize = 0xAA000;
constexpr uint16_t kXvcKeyNone = 0xFFFF;
constexpr uint32_t kMaxXvcRegions = 0x1000;

namespace XvdVolumeFlags
{
constexpr uint32_t kReadOnly = 0x01;
constexpr uint32_t kEncryptionDisabled = 0x02;
constexpr uint32_t kDataIntegrityDisabled = 0x04;
constexpr uint32_t kLegacySectorSize = 0x08;
constexpr uint32_t kResiliencyEnabled = 0x10;
constexpr uint32_t kSraReadOnly = 0x20;
constexpr uint32_t kRegionIdInXts = 0x40;
constexpr uint32_t kEraSpecific = 0x80;
constexpr uint32_t kKnownMask = 0xFF;
} // namespace XvdVolumeFlags

enum class XvdContainerType : uint8_t
{
    Xvd,
    Xvc,
};

enum class XvdVolumeType : uint8_t
{
    Fixed,
    Dynamic,
};

enum class XvcRegionId : uint32_t
{
    MetadataXvc = 0x4000'0001,
    MetadataFilesystem = 0x4000'0002,
    Unknown = 0x4000'0003,
    EmbeddedXvd = 0x4000'0004,
    Header = 0x4000'0005,
    MutableData = 0x4000'0006,
};

#pragma pack(push, 1)

// Follows the 0x200-byte signature; offsets are relative to Magic.
struct XvdHeader
{
    uint64_t Magic;
    uint32_t VolumeFlags;
    uint32_t FormatVersion;
    uint64_t CreationTime; // FILETIME
    uint64_t DriveSize;
    std::array<uint8_t, 16> VolumeId;
    std::array<uint8_t, 16> UserVolumeId;
    std::array<uint8_t, 32> TopHashBlockHash;
    std::array<uint8_t, 32> OriginalXvcDataHash;
    uint32_t VolumeType;
    uint32_t ContentType;
    uint32_t EmbeddedXvdLength;
    uint32_t UserDataLength;
    uint32_t XvcDataLength;
    uint32_t DynamicHeaderLength;
    uint32_t BlockSize;
    std::array<uint8_t, 0x60> ExtEntries;
    std::array<uint16_t, 8> Capabilities;
    std::array<uint8_t, 32> PeCatalogHash;
    std::array<uint8_t, 16> EmbeddedXvdBuildId;
    std::array<uint8_t, 16> Reserved13C;
    std::array<uint8_t, 32> KeyMaterial;
    std::array<uint8_t, 32> UserDataHash;
    std::array<char, 16> SandboxId;
    std::array<uint8_t, 16> ProductId;
    std::array<uint8_t, 16> BuildId;
    std::array<uint16_t, 4> PackageVersion;
    std::array<uint16_t, 16> PeCatalogCaps;
    std::array<uint8_t, 0x80> PeCatalogs;
    uint32_t WriteableExpirationDate;
    uint32_t WriteablePolicyFlags;
    uint32_t PersistentLocalStorageSize;
    uint8_t MutableDataPageCount;
    uint8_t Unknown271;
    std::array<uint8_t, 16> Unknown272;
    std::array<uint8_t, 10> Reserved282;
    uint64_t SequenceNumber;
    std::array<uint16_t, 4> RequiredSystemVersion;
    uint32_t OdkKeySlot;
    std::array<uint8_t, 0xB54> Reserved2A0;
    uint64_t ResilientDataOffset;
    uint32_t ResilientDataLength;
};
static_assert(sizeof(XvdHeader) == 0xE00);
static_assert(offsetof(XvdHeader, VolumeType) == 0x80);
static_assert(offsetof(XvdHeader, BlockSize) == 0x98);
static_assert(offsetof(XvdHeader, SandboxId) == 0x18C);
static_assert(offsetof(XvdHeader, MutableDataPageCount) == 0x270);
static_assert(offsetof(XvdHeader, SequenceNumber) == 0x28C);
static_assert(offsetof(XvdHeader, ResilientDataOffset) == 0xDF4);

struct XvcInfo
{
    std::array<uint8_t, 16> ContentId;
    std::array<uint8_t, 0xC00> EncryptionKeyIds;
    std::array<uint8_t, 0x100> Description; // UTF-16
    uint32_t Version;
    uint32_t RegionCount;
    uint32_t Flags;
    uint16_t Padding;
    uint16_t KeyCount;
    uint32_t UnknownD20;
    uint32_t InitialPlayRegionId;
    uint64_t InitialPlayOffset;
    uint64_t CreationTime;
    uint32_t PreviewRegionId;
    uint32_t UpdateSegmentCount;
    uint64_t PreviewOffset;
    uint64_t UnusedSpace;
    uint32_t RegionSpecifierCount;
    std::array<uint8_t, 0x54> Reserved;
};
static_assert(sizeof(XvcInfo) == 0xDA8);
static_assert(offsetof(XvcInfo, Version) == 0xD10);
static_assert(offsetof(XvcInfo, KeyCount) == 0xD1E);
static_assert(offsetof(XvcInfo, RegionSpecifierCount) == 0xD50);

struct XvcRegionHeader
{
    uint32_t Id;
    uint16_t KeyId;
    uint16_t Padding;
    uint32_t Flags;
    uint32_t FirstSegmentIndex;
    std::array<char16_t, 32> Description;
    uint64_t Offset;
    uint64_t Length;
    uint64_t Hash;
    std::array<uint8_t, 0x18> Unknown;
};
static_assert(sizeof(XvcRegionHeader) == 0x80);
static_assert(offsetof(XvcRegionHeader, Offset) == 0x50);
static_assert(offsetof(XvcRegionHeader, Hash) == 0x60);

#pragma pack(pop)

// Absolute file offsets of each container section, in file order.
struct XvdSectionLayout
{
    uint64_t EmbeddedXvdOffset = 0;
    uint64_t MutableDataOffset = 0;
    uint64_t HashTreeOffset = 0;
    uint64_t UserDataOffset = 0;
    uint64_t XvcInfoOffset = 0;
    uint64_t DynamicHeaderOffset = 0;
    uint64_t DriveDataOffset = 0;
    uint64_t HashTreePageCount = 0;
};

struct XvdHeaderSummary
{
    XvdContainerType ContainerType = XvdContainerType::Xvd;
    XvdVolumeType VolumeType = XvdVolumeType::Fixed;
    uint32_t FormatVersion = 0;
    uint32_t VolumeFlags = 0;
    uint32_t ContentType = 0;
    uint64_t DriveSize = 0;
    uint64_t CreationTime = 0;
    std::array<uint8_t, 16> VolumeId{};
    uint32_t XvcDataLength = 0;
    XvdSectionLayout Layout;
};

[[nodiscard]] constexpr bool IsXvcContentType(uint32_t contentType) noexcept
{
    // Title, Application, MteApp, MteTitle, AppDlc, TitleDlc, UniversalDlc.
    constexpr uint32_t kXvcContentTypeMask = 0x0701'8042;
    return contentType < 32 && ((kXvcContentTypeMask >> contentType) & 1U) != 0;
}

namespace Detail
{
constexpr uint64_t CeilDiv(uint64_t value, uint64_t divisor) noexcept
{
    return (value / divisor) + (value % divisor != 0 ? 1 : 0);
}

constexpr uint64_t HashBlocksInLevel(uint64_t hashedPageCount, uint64_t level) noexcept
{
    switch (level)
    {
    case 0:
        return CeilDiv(hashedPageCount, 0xAA);
    case 1:
        return CeilDiv(hashedPageCount, 0x70E4);
    case 2:
        return CeilDiv(hashedPageCount, 0x4AF768);
    case 3:
        return CeilDiv(hashedPageCount, 0x31C84B10);
    default:
        return 0;
    }
}
} // namespace Detail

[[nodiscard]] constexpr uint64_t XvdHashTreePageCount(uint64_t hashedPageCount, bool resilient) noexcept
{
    uint64_t pageCount = Detail::HashBlocksInLevel(hashedPageCount, 0);
    if (pageCount > 1)
    {
        uint64_t levelBlocks = 2;
        for (uint64_t level = 1; levelBlocks > 1; ++level)
        {
            levelBlocks = Detail::HashBlocksInLevel(hashedPageCount, level);
            pageCount += levelBlocks;
        }
    }
    return resilient ? pageCount * 2 : pageCount;
}

class XvdReader
{
public:
    explicit XvdReader(std::span<const std::byte> file) noexcept;
    XvdReader(std::span<const std::byte> file, Core::Logger& logger) noexcept;

    [[nodiscard]] std::optional<XvdHeader> ReadRawHeader() const noexcept;
    [[nodiscard]] std::optional<XvdHeaderSummary> ReadHeader() const noexcept;

    // Empty for non-XVC containers or XVC info version 0.
    [[nodiscard]] std::optional<std::vector<XvcRegionHeader>> ReadXvcRegions(
        const XvdHeaderSummary& summary) const noexcept;

private:
    std::span<const std::byte> m_file;
    std::reference_wrapper<Core::Logger> m_logger;
};
} // namespace OpenEra::Loader
