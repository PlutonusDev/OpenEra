#include "XvdReader.h"

#include <limits>

#include "Core/ByteReader.h"
#include "Core/Logger.h"

namespace OpenEra::Loader
{
namespace
{
using Core::ByteReader;
using Core::Logger;
using Detail::CeilDiv;

constexpr uint64_t PageBytes(uint64_t byteLength) noexcept
{
    return CeilDiv(byteLength, kXvdPageSize) * kXvdPageSize;
}

// Section lengths are 32-bit and hash pages are bounded by DriveSize / 0x1000,
// so these sums cannot overflow 64 bits.
XvdSectionLayout ComputeLayout(const XvdHeader& header) noexcept
{
    const bool integrity = (header.VolumeFlags & XvdVolumeFlags::kDataIntegrityDisabled) == 0;
    const bool resilient = (header.VolumeFlags & XvdVolumeFlags::kResiliencyEnabled) != 0;
    const uint64_t hashedPageCount =
        CeilDiv(header.DriveSize, kXvdPageSize) + CeilDiv(header.UserDataLength, kXvdPageSize) +
        CeilDiv(header.XvcDataLength, kXvdPageSize) + CeilDiv(header.DynamicHeaderLength, kXvdPageSize);

    XvdSectionLayout layout;
    layout.HashTreePageCount = integrity ? XvdHashTreePageCount(hashedPageCount, resilient) : 0;
    layout.EmbeddedXvdOffset = kXvdHeaderRegionSize;
    layout.MutableDataOffset = layout.EmbeddedXvdOffset + PageBytes(header.EmbeddedXvdLength);
    layout.HashTreeOffset = layout.MutableDataOffset + (header.MutableDataPageCount * kXvdPageSize);
    layout.UserDataOffset = layout.HashTreeOffset + (layout.HashTreePageCount * kXvdPageSize);
    layout.XvcInfoOffset = layout.UserDataOffset + PageBytes(header.UserDataLength);
    layout.DynamicHeaderOffset = layout.XvcInfoOffset + PageBytes(header.XvcDataLength);
    layout.DriveDataOffset = layout.DynamicHeaderOffset + PageBytes(header.DynamicHeaderLength);
    return layout;
}

std::optional<XvdHeader> ReadRawHeaderImpl(std::span<const std::byte> file, Logger& logger)
{
    ByteReader reader(file);
    std::optional<XvdHeader> header;
    if (reader.Skip(kXvdSignatureSize))
    {
        header = reader.ReadStruct<XvdHeader>();
    }
    if (!header)
    {
        logger.Warn("XVD: truncated header ({} bytes, need {})", file.size(), kXvdSignatureSize + sizeof(XvdHeader));
    }
    return header;
}

std::optional<XvdHeaderSummary> ReadHeaderImpl(std::span<const std::byte> file, Logger& logger)
{
    const auto header = ReadRawHeaderImpl(file, logger);
    if (!header)
    {
        return std::nullopt;
    }

    // Packed fields are copied before logging; the logger takes arguments by reference.
    const uint64_t magic = header->Magic;
    const uint32_t volumeType = header->VolumeType;
    const uint32_t blockSize = header->BlockSize;
    const uint32_t volumeFlags = header->VolumeFlags;
    const uint32_t formatVersion = header->FormatVersion;
    if (magic != kXvdMagic)
    {
        logger.Error("XVD: bad magic {:#018x}", magic);
        return std::nullopt;
    }
    if (volumeType > 1)
    {
        logger.Error("XVD: unknown volume type {}", volumeType);
        return std::nullopt;
    }
    if (blockSize != kXvdBlockSize)
    {
        logger.Error("XVD: unsupported block size {:#x}", blockSize);
        return std::nullopt;
    }
    if ((volumeFlags & ~XvdVolumeFlags::kKnownMask) != 0)
    {
        logger.Warn("XVD: unknown volume flags {:#x}", volumeFlags);
    }
    if (formatVersion != 3)
    {
        logger.Warn("XVD: unexpected format version {}", formatVersion);
    }

    XvdHeaderSummary summary;
    summary.ContainerType = IsXvcContentType(header->ContentType) && header->XvcDataLength > 0 ? XvdContainerType::Xvc
                                                                                               : XvdContainerType::Xvd;
    summary.VolumeType = volumeType == 0 ? XvdVolumeType::Fixed : XvdVolumeType::Dynamic;
    summary.FormatVersion = formatVersion;
    summary.VolumeFlags = volumeFlags;
    summary.ContentType = header->ContentType;
    summary.DriveSize = header->DriveSize;
    summary.CreationTime = header->CreationTime;
    summary.VolumeId = header->VolumeId;
    summary.XvcDataLength = header->XvcDataLength;
    summary.Layout = ComputeLayout(*header);
    return summary;
}

std::optional<std::vector<XvcRegionHeader>> ReadXvcRegionsImpl(std::span<const std::byte> file, Logger& logger,
                                                               const XvdHeaderSummary& summary)
{
    std::vector<XvcRegionHeader> regions;
    if (summary.ContainerType != XvdContainerType::Xvc)
    {
        return regions;
    }

    ByteReader reader(file);
    std::optional<XvcInfo> info;
    if (reader.Seek(summary.Layout.XvcInfoOffset))
    {
        info = reader.ReadStruct<XvcInfo>();
    }
    if (!info)
    {
        logger.Warn("XVC: truncated info at {:#x}", summary.Layout.XvcInfoOffset);
        return std::nullopt;
    }
    if (info->Version == 0)
    {
        return regions;
    }

    const uint32_t regionCount = info->RegionCount;
    const uint16_t keyCount = info->KeyCount;
    if (regionCount > kMaxXvcRegions)
    {
        logger.Error("XVC: region count {} exceeds limit {}", regionCount, kMaxXvcRegions);
        return std::nullopt;
    }
    if (sizeof(XvcInfo) + (regionCount * sizeof(XvcRegionHeader)) > summary.XvcDataLength)
    {
        logger.Error("XVC: {} regions do not fit in {:#x} bytes of XVC data", regionCount, summary.XvcDataLength);
        return std::nullopt;
    }

    regions.reserve(regionCount);
    for (uint32_t index = 0; index < regionCount; ++index)
    {
        const auto region = reader.ReadStruct<XvcRegionHeader>();
        if (!region)
        {
            logger.Warn("XVC: truncated region table at entry {}", index);
            return std::nullopt;
        }
        const uint16_t keyId = region->KeyId;
        if (keyId != kXvcKeyNone && keyId >= keyCount)
        {
            logger.Error("XVC: region {} uses key {} of {}", index, keyId, keyCount);
            return std::nullopt;
        }
        if (region->Length > std::numeric_limits<uint64_t>::max() - region->Offset)
        {
            logger.Error("XVC: region {} range overflows", index);
            return std::nullopt;
        }
        regions.push_back(*region);
    }
    return regions;
}
} // namespace

XvdReader::XvdReader(std::span<const std::byte> file) noexcept
    : XvdReader(file, Core::DefaultLogger())
{
}

XvdReader::XvdReader(std::span<const std::byte> file, Core::Logger& logger) noexcept
    : m_file(file)
    , m_logger(logger)
{
}

// The public methods are the noexcept boundary; allocation failure reads as nullopt.
std::optional<XvdHeader> XvdReader::ReadRawHeader() const noexcept
{
    try
    {
        return ReadRawHeaderImpl(m_file, m_logger.get());
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<XvdHeaderSummary> XvdReader::ReadHeader() const noexcept
{
    try
    {
        return ReadHeaderImpl(m_file, m_logger.get());
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<std::vector<XvcRegionHeader>> XvdReader::ReadXvcRegions(const XvdHeaderSummary& summary) const noexcept
{
    try
    {
        return ReadXvcRegionsImpl(m_file, m_logger.get(), summary);
    }
    catch (...)
    {
        return std::nullopt;
    }
}
} // namespace OpenEra::Loader
