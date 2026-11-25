#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"
#include "Loader/XvdReader.h"

using namespace OpenEra::Loader;
using OpenEra::Core::Logger;
using OpenEra::Core::LogLevel;
using OpenEra::Core::LogRecord;
using OpenEra::Core::LogSink;

namespace
{
// Synthetic XVC, data integrity disabled, so sections pack without a hash tree:
//   0x0000 signature, 0x0200 header, 0x3000 user data (1 page),
//   0x4000 XVC info + regions (3 pages), 0x7000 drive data.
constexpr uint64_t kUserDataOffset = 0x3000;
constexpr uint64_t kXvcInfoOffset = 0x4000;
constexpr uint64_t kRegionTableOffset = kXvcInfoOffset + sizeof(XvcInfo);
constexpr uint64_t kDriveDataOffset = 0x7000;
constexpr uint32_t kXvcDataLength = 0x3000;
constexpr size_t kFileSize = 0x7000;

class CaptureSink final : public LogSink
{
public:
    void Write(const LogRecord& record) override { m_records.emplace_back(record.Level, record.Message); }

    [[nodiscard]] bool Contains(LogLevel level, std::string_view text) const
    {
        return std::ranges::any_of(m_records, [&](const auto& entry)
                                   { return entry.first == level && entry.second.find(text) != std::string::npos; });
    }

    [[nodiscard]] size_t Count() const noexcept { return m_records.size(); }

private:
    std::vector<std::pair<LogLevel, std::string>> m_records;
};

struct TestLogger
{
    Logger Log;
    std::shared_ptr<CaptureSink> Sink = std::make_shared<CaptureSink>();

    TestLogger()
    {
        Log.SetMinimumLevel(LogLevel::Trace);
        Log.AddSink(Sink);
    }
};

template <typename T>
void Put(std::vector<std::byte>& bytes, uint64_t offset, const T& value)
{
    const auto raw = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    if (offset > bytes.size() || bytes.size() - offset < raw.size())
    {
        throw std::out_of_range("synthetic XVD write out of range");
    }
    std::ranges::copy(raw, std::span(bytes).subspan(offset).begin());
}

XvcRegionHeader MakeRegion(uint32_t id, uint16_t keyId, uint64_t offset, uint64_t length, std::u16string_view name)
{
    XvcRegionHeader region{};
    region.Id = id;
    region.KeyId = keyId;
    region.Offset = offset;
    region.Length = length;
    std::ranges::copy(name.substr(0, region.Description.size()), region.Description.begin());
    return region;
}

struct XvdBuilder
{
    XvdHeader Header{};
    XvcInfo Info{};
    std::vector<XvcRegionHeader> Regions;

    XvdBuilder()
    {
        Header.Magic = kXvdMagic;
        Header.VolumeFlags = XvdVolumeFlags::kDataIntegrityDisabled;
        Header.FormatVersion = 3;
        Header.CreationTime = 0x01DA'0000'0000'0000;
        Header.DriveSize = 0x10000;
        Header.VolumeId = {0x11, 0x22, 0x33, 0x44};
        Header.VolumeType = 0;
        Header.ContentType = 1; // Title
        Header.UserDataLength = 0x1000;
        Header.XvcDataLength = kXvcDataLength;
        Header.BlockSize = kXvdBlockSize;

        Info.Version = 1;
        Info.KeyCount = 1;
        Regions = {
            MakeRegion(static_cast<uint32_t>(XvcRegionId::Header), kXvcKeyNone, 0, 0x3000, u"XVC-HD"),
            MakeRegion(static_cast<uint32_t>(XvcRegionId::MetadataXvc), kXvcKeyNone, kXvcInfoOffset, kXvcDataLength,
                       u"XVC-MD"),
            MakeRegion(0x1234, 0, kDriveDataOffset, 0x10000, u"Game"),
        };
    }

    [[nodiscard]] std::vector<std::byte> Build() const
    {
        std::vector<std::byte> bytes(kFileSize);
        Put(bytes, kXvdSignatureSize, Header);

        auto info = Info;
        info.RegionCount = static_cast<uint32_t>(Regions.size());
        Put(bytes, kXvcInfoOffset, info);
        uint64_t offset = kRegionTableOffset;
        for (const auto& region : Regions)
        {
            Put(bytes, offset, region);
            offset += sizeof(XvcRegionHeader);
        }
        return bytes;
    }
};
} // namespace

TEST_CASE("XVD structures match the documented layout", "[XvdReader]")
{
    STATIC_REQUIRE(sizeof(XvdHeader) == 0xE00);
    STATIC_REQUIRE(sizeof(XvcInfo) == 0xDA8);
    STATIC_REQUIRE(sizeof(XvcRegionHeader) == 0x80);
    STATIC_REQUIRE(offsetof(XvdHeader, DriveSize) == 0x18);
    STATIC_REQUIRE(offsetof(XvdHeader, ContentType) == 0x84);
    STATIC_REQUIRE(offsetof(XvdHeader, XvcDataLength) == 0x90);
    STATIC_REQUIRE(offsetof(XvdHeader, PackageVersion) == 0x1BC);
    STATIC_REQUIRE(offsetof(XvdHeader, OdkKeySlot) == 0x29C);
    STATIC_REQUIRE(offsetof(XvcInfo, RegionCount) == 0xD14);
    STATIC_REQUIRE(offsetof(XvcRegionHeader, Flags) == 0x08);
    STATIC_REQUIRE(offsetof(XvcRegionHeader, Description) == 0x10);
    STATIC_REQUIRE(offsetof(XvcRegionHeader, Length) == 0x58);
    STATIC_REQUIRE(kXvdMagic == 0x6476'782D'7466'736D);
}

TEST_CASE("IsXvcContentType matches the XVC-enabled content types", "[XvdReader]")
{
    for (const uint32_t type : {0x01U, 0x06U, 0x0FU, 0x10U, 0x18U, 0x19U, 0x1AU})
    {
        CHECK(IsXvcContentType(type));
    }
    for (const uint32_t type : {0x00U, 0x02U, 0x03U, 0x05U, 0x1BU, 0x1FU, 32U, 0xFFFF'FFFFU})
    {
        CHECK_FALSE(IsXvcContentType(type));
    }
}

TEST_CASE("XvdHashTreePageCount", "[XvdReader]")
{
    CHECK(XvdHashTreePageCount(0, false) == 0);
    CHECK(XvdHashTreePageCount(1, false) == 1);
    CHECK(XvdHashTreePageCount(0xAA, false) == 1);
    CHECK(XvdHashTreePageCount(0xAB, false) == 3);
    CHECK(XvdHashTreePageCount(0xAB, true) == 6);
    CHECK(XvdHashTreePageCount(0x70E4, false) == 0xAA + 1);
    CHECK(XvdHashTreePageCount(0x70E5, false) == 0xAB + 2 + 1);
    // Terminates past the deepest level.
    CHECK(XvdHashTreePageCount(std::numeric_limits<uint64_t>::max() / kXvdPageSize, false) > 0);
}

TEST_CASE("ReadHeader parses a valid XVC header", "[XvdReader]")
{
    TestLogger logger;
    const auto bytes = XvdBuilder{}.Build();
    const auto summary = XvdReader(bytes, logger.Log).ReadHeader();
    REQUIRE(summary.has_value());

    CHECK(summary->ContainerType == XvdContainerType::Xvc);
    CHECK(summary->VolumeType == XvdVolumeType::Fixed);
    CHECK(summary->FormatVersion == 3);
    CHECK(summary->VolumeFlags == XvdVolumeFlags::kDataIntegrityDisabled);
    CHECK(summary->ContentType == 1);
    CHECK(summary->DriveSize == 0x10000);
    CHECK(summary->CreationTime == 0x01DA'0000'0000'0000);
    CHECK(summary->VolumeId.at(3) == 0x44);

    const auto& layout = summary->Layout;
    CHECK(layout.EmbeddedXvdOffset == kXvdHeaderRegionSize);
    CHECK(layout.MutableDataOffset == kXvdHeaderRegionSize);
    CHECK(layout.HashTreeOffset == kXvdHeaderRegionSize);
    CHECK(layout.HashTreePageCount == 0);
    CHECK(layout.UserDataOffset == kUserDataOffset);
    CHECK(layout.XvcInfoOffset == kXvcInfoOffset);
    CHECK(layout.DynamicHeaderOffset == kDriveDataOffset);
    CHECK(layout.DriveDataOffset == kDriveDataOffset);
    CHECK(logger.Sink->Count() == 0);
}

TEST_CASE("ReadHeader lays out embedded XVD, mutable data and hash tree", "[XvdReader]")
{
    XvdBuilder builder;
    builder.Header.VolumeType = 1;
    builder.Header.VolumeFlags = 0;
    builder.Header.EmbeddedXvdLength = 0x1800; // 2 pages
    builder.Header.MutableDataPageCount = 1;
    builder.Header.DynamicHeaderLength = 0x10;
    // Hashed pages: drive 16 + user 1 + XVC 3 + dynamic 1 = 21 -> 1 hash page.

    SECTION("integrity enabled")
    {
        const auto summary = XvdReader(builder.Build()).ReadHeader();
        REQUIRE(summary.has_value());
        const auto& layout = summary->Layout;
        CHECK(summary->VolumeType == XvdVolumeType::Dynamic);
        CHECK(layout.MutableDataOffset == 0x5000);
        CHECK(layout.HashTreeOffset == 0x6000);
        CHECK(layout.HashTreePageCount == 1);
        CHECK(layout.UserDataOffset == 0x7000);
        CHECK(layout.XvcInfoOffset == 0x8000);
        CHECK(layout.DynamicHeaderOffset == 0xB000);
        CHECK(layout.DriveDataOffset == 0xC000);
    }

    SECTION("resilient hash tree is doubled")
    {
        builder.Header.VolumeFlags = XvdVolumeFlags::kResiliencyEnabled;
        const auto summary = XvdReader(builder.Build()).ReadHeader();
        REQUIRE(summary.has_value());
        CHECK(summary->Layout.HashTreePageCount == 2);
        CHECK(summary->Layout.UserDataOffset == 0x8000);
        CHECK(summary->Layout.DriveDataOffset == 0xD000);
    }
}

TEST_CASE("ReadHeader classifies non-XVC containers as XVD", "[XvdReader]")
{
    XvdBuilder builder;

    SECTION("non-XVC content type")
    {
        builder.Header.ContentType = 0x02; // SystemOS
    }

    SECTION("XVC content type without XVC data")
    {
        builder.Header.XvcDataLength = 0;
    }

    const auto bytes = builder.Build();
    const XvdReader reader(bytes);
    const auto summary = reader.ReadHeader();
    REQUIRE(summary.has_value());
    CHECK(summary->ContainerType == XvdContainerType::Xvd);

    const auto regions = reader.ReadXvcRegions(*summary);
    REQUIRE(regions.has_value());
    CHECK(regions->empty());
}

TEST_CASE("ReadHeader rejects truncated and corrupt headers", "[XvdReader]")
{
    TestLogger logger;
    XvdBuilder builder;
    auto bytes = builder.Build();

    SECTION("empty buffer")
    {
        bytes.clear();
        CHECK_FALSE(XvdReader(bytes, logger.Log).ReadHeader().has_value());
        CHECK(logger.Sink->Contains(LogLevel::Warn, "truncated header"));
    }

    SECTION("signature only")
    {
        bytes.resize(kXvdSignatureSize);
        CHECK_FALSE(XvdReader(bytes, logger.Log).ReadHeader().has_value());
    }

    SECTION("one byte short of a full header")
    {
        bytes.resize(kXvdSignatureSize + sizeof(XvdHeader) - 1);
        CHECK_FALSE(XvdReader(bytes, logger.Log).ReadRawHeader().has_value());
        bytes.push_back(std::byte{0});
        CHECK(XvdReader(bytes, logger.Log).ReadHeader().has_value());
    }

    SECTION("bad magic")
    {
        builder.Header.Magic = 0x6476'782D'7466'736E;
        CHECK_FALSE(XvdReader(builder.Build(), logger.Log).ReadHeader().has_value());
        CHECK(logger.Sink->Contains(LogLevel::Error, "bad magic"));
    }

    SECTION("unknown volume type")
    {
        builder.Header.VolumeType = 2;
        CHECK_FALSE(XvdReader(builder.Build(), logger.Log).ReadHeader().has_value());
        CHECK(logger.Sink->Contains(LogLevel::Error, "volume type"));
    }

    SECTION("unsupported block size")
    {
        builder.Header.BlockSize = 0x1000;
        CHECK_FALSE(XvdReader(builder.Build(), logger.Log).ReadHeader().has_value());
        CHECK(logger.Sink->Contains(LogLevel::Error, "block size"));
    }
}

TEST_CASE("ReadHeader warns on unknown flags and versions", "[XvdReader]")
{
    TestLogger logger;
    XvdBuilder builder;
    builder.Header.VolumeFlags |= 0x100;
    builder.Header.FormatVersion = 2;

    const auto summary = XvdReader(builder.Build(), logger.Log).ReadHeader();
    REQUIRE(summary.has_value());
    CHECK(summary->VolumeFlags == (XvdVolumeFlags::kDataIntegrityDisabled | 0x100));
    CHECK(logger.Sink->Contains(LogLevel::Warn, "volume flags"));
    CHECK(logger.Sink->Contains(LogLevel::Warn, "format version"));
}

TEST_CASE("ReadXvcRegions parses the region table", "[XvdReader]")
{
    const auto bytes = XvdBuilder{}.Build();
    const XvdReader reader(bytes);
    const auto summary = reader.ReadHeader();
    REQUIRE(summary.has_value());

    const auto regions = reader.ReadXvcRegions(*summary);
    REQUIRE(regions.has_value());
    REQUIRE(regions->size() == 3);

    CHECK(regions->at(0).Id == static_cast<uint32_t>(XvcRegionId::Header));
    CHECK(regions->at(0).KeyId == kXvcKeyNone);
    CHECK(regions->at(0).Length == 0x3000);
    CHECK(regions->at(1).Id == static_cast<uint32_t>(XvcRegionId::MetadataXvc));
    CHECK(regions->at(1).Offset == kXvcInfoOffset);

    const auto& game = regions->at(2);
    CHECK(game.Id == 0x1234);
    CHECK(game.KeyId == 0);
    CHECK(game.Offset == kDriveDataOffset);
    CHECK(game.Length == 0x10000);
    CHECK(std::u16string_view(game.Description.data(), 4) == u"Game");
}

TEST_CASE("ReadXvcRegions boundary conditions", "[XvdReader]")
{
    TestLogger logger;
    XvdBuilder builder;

    SECTION("version 0 has no region table")
    {
        builder.Info.Version = 0;
        const auto bytes = builder.Build();
        const XvdReader reader(bytes, logger.Log);
        const auto regions = reader.ReadXvcRegions(reader.ReadHeader().value());
        REQUIRE(regions.has_value());
        CHECK(regions->empty());
    }

    SECTION("empty region table")
    {
        builder.Regions.clear();
        const auto bytes = builder.Build();
        const XvdReader reader(bytes, logger.Log);
        const auto regions = reader.ReadXvcRegions(reader.ReadHeader().value());
        REQUIRE(regions.has_value());
        CHECK(regions->empty());
    }

    SECTION("region table exactly fills the XVC data")
    {
        builder.Header.XvcDataLength = sizeof(XvcInfo) + (3 * sizeof(XvcRegionHeader));
        const auto bytes = builder.Build();
        const XvdReader reader(bytes, logger.Log);
        const auto regions = reader.ReadXvcRegions(reader.ReadHeader().value());
        REQUIRE(regions.has_value());
        CHECK(regions->size() == 3);
    }

    SECTION("last key id is in range")
    {
        builder.Info.KeyCount = 2;
        builder.Regions.at(2).KeyId = 1;
        const auto bytes = builder.Build();
        const XvdReader reader(bytes, logger.Log);
        CHECK(reader.ReadXvcRegions(reader.ReadHeader().value()).has_value());
    }

    CHECK(logger.Sink->Count() == 0);
}

TEST_CASE("ReadXvcRegions rejects corrupt region tables", "[XvdReader]")
{
    TestLogger logger;
    XvdBuilder builder;
    const auto readRegions = [&](const std::vector<std::byte>& bytes)
    {
        const XvdReader reader(bytes, logger.Log);
        const auto summary = reader.ReadHeader();
        REQUIRE(summary.has_value());
        return reader.ReadXvcRegions(*summary);
    };

    SECTION("region count above the limit")
    {
        auto bytes = builder.Build();
        Put(bytes, kXvcInfoOffset + offsetof(XvcInfo, RegionCount), kMaxXvcRegions + 1);
        CHECK_FALSE(readRegions(bytes).has_value());
        CHECK(logger.Sink->Contains(LogLevel::Error, "exceeds limit"));
    }

    SECTION("region table larger than the XVC data")
    {
        builder.Header.XvcDataLength = sizeof(XvcInfo) + (3 * sizeof(XvcRegionHeader)) - 1;
        CHECK_FALSE(readRegions(builder.Build()).has_value());
        CHECK(logger.Sink->Contains(LogLevel::Error, "do not fit"));
    }

    SECTION("file ends inside the region table")
    {
        auto bytes = builder.Build();
        bytes.resize(kRegionTableOffset + sizeof(XvcRegionHeader) + 8);
        CHECK_FALSE(readRegions(bytes).has_value());
        CHECK(logger.Sink->Contains(LogLevel::Warn, "truncated region table at entry 1"));
    }

    SECTION("file ends before the XVC info")
    {
        auto bytes = builder.Build();
        bytes.resize(kXvcInfoOffset + 0x10);
        CHECK_FALSE(readRegions(bytes).has_value());
        CHECK(logger.Sink->Contains(LogLevel::Warn, "truncated info"));
    }

    SECTION("key id out of range")
    {
        builder.Regions.at(2).KeyId = 1;
        CHECK_FALSE(readRegions(builder.Build()).has_value());
        CHECK(logger.Sink->Contains(LogLevel::Error, "uses key 1 of 1"));
    }

    SECTION("offset plus length overflows")
    {
        builder.Regions.at(2).Offset = std::numeric_limits<uint64_t>::max() - 0xF;
        builder.Regions.at(2).Length = 0x10;
        CHECK_FALSE(readRegions(builder.Build()).has_value());
        CHECK(logger.Sink->Contains(LogLevel::Error, "overflows"));
    }
}

TEST_CASE("XvdReader never reads past a truncated buffer", "[XvdReader]")
{
    TestLogger logger;
    const auto full = XvdBuilder{}.Build();
    const auto fullSpan = std::span(full);

    for (size_t length = 0; length <= full.size(); length += 8)
    {
        INFO("length = " << length);
        const XvdReader reader(fullSpan.first(length), logger.Log);
        const auto summary = reader.ReadHeader();
        REQUIRE(summary.has_value() == (length >= kXvdSignatureSize + sizeof(XvdHeader)));
        if (summary)
        {
            const auto regions = reader.ReadXvcRegions(*summary);
            REQUIRE(regions.has_value() == (length >= kRegionTableOffset + (3 * sizeof(XvcRegionHeader))));
        }
    }
}
