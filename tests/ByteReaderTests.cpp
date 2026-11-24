#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Core/ByteReader.h"

using OpenEra::Core::ByteReader;

namespace
{
std::vector<std::byte> MakeBuffer(std::initializer_list<uint8_t> values)
{
    std::vector<std::byte> buffer;
    buffer.reserve(values.size());
    for (const uint8_t value : values)
    {
        buffer.push_back(static_cast<std::byte>(value));
    }
    return buffer;
}

#pragma pack(push, 1)
struct TestHeader
{
    uint32_t Magic;
    uint16_t Version;
    uint16_t Flags;
    uint64_t ContentSize;
};

// Odd-sized record: without packing this would be 8 bytes.
struct TestRecord
{
    uint8_t Kind;
    uint32_t Value;
};
#pragma pack(pop)

static_assert(sizeof(TestHeader) == 16);
static_assert(offsetof(TestHeader, Magic) == 0);
static_assert(offsetof(TestHeader, Version) == 4);
static_assert(offsetof(TestHeader, Flags) == 6);
static_assert(offsetof(TestHeader, ContentSize) == 8);

static_assert(sizeof(TestRecord) == 5);
static_assert(offsetof(TestRecord, Kind) == 0);
static_assert(offsetof(TestRecord, Value) == 1);

enum class TestKind : uint16_t
{
    None = 0,
    Package = 0x0102,
};

// ReadStruct<T> must only accept trivially copyable types.
template <typename T>
concept CanReadStruct = requires(ByteReader reader) { reader.ReadStruct<T>(); };

static_assert(CanReadStruct<TestHeader>);
static_assert(CanReadStruct<TestKind>);
static_assert(CanReadStruct<uint64_t>);
static_assert(!CanReadStruct<std::string>);
static_assert(!CanReadStruct<std::vector<std::byte>>);
} // namespace

TEST_CASE("Default-constructed reader is empty", "[ByteReader]")
{
    ByteReader reader;

    CHECK(reader.Size() == 0);
    CHECK(reader.Offset() == 0);
    CHECK(reader.Remaining() == 0);
    CHECK_FALSE(reader.ReadU8().has_value());
    CHECK(reader.Seek(0));
    CHECK_FALSE(reader.Seek(1));
    CHECK(reader.Skip(0));
    CHECK_FALSE(reader.Skip(1));

    const auto empty = reader.ReadBytes(0);
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("Integers are read little-endian in sequence", "[ByteReader]")
{
    const auto buffer = MakeBuffer({
        0xAB, // U8
        0x34,
        0x12, // U16 0x1234
        0x78,
        0x56,
        0x34,
        0x12, // U32 0x12345678
        0xEF,
        0xCD,
        0xAB,
        0x89,
        0x67,
        0x45,
        0x23,
        0x01, // U64 0x0123456789ABCDEF
    });
    ByteReader reader(buffer);

    REQUIRE(reader.Size() == 15);

    CHECK(reader.ReadU8() == uint8_t{0xAB});
    CHECK(reader.Offset() == 1);

    CHECK(reader.ReadU16() == uint16_t{0x1234});
    CHECK(reader.Offset() == 3);

    CHECK(reader.ReadU32() == uint32_t{0x12345678});
    CHECK(reader.Offset() == 7);

    CHECK(reader.ReadU64() == uint64_t{0x0123456789ABCDEF});
    CHECK(reader.Offset() == 15);
    CHECK(reader.Remaining() == 0);
}

TEST_CASE("Maximum values round-trip", "[ByteReader]")
{
    const auto buffer = MakeBuffer({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    ByteReader reader(buffer);

    CHECK(reader.ReadU64() == std::numeric_limits<uint64_t>::max());
}

TEST_CASE("ReadBytes returns a view into the original buffer", "[ByteReader]")
{
    const auto buffer = MakeBuffer({0x10, 0x20, 0x30, 0x40, 0x50});
    ByteReader reader(buffer);

    REQUIRE(reader.Skip(1));
    const auto bytes = reader.ReadBytes(3);

    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() == 3);
    CHECK(bytes->data() == &buffer.at(1));
    CHECK(bytes->front() == std::byte{0x20});
    CHECK(bytes->back() == std::byte{0x40});
    CHECK(reader.Offset() == 4);
    CHECK(reader.Remaining() == 1);
}

TEST_CASE("Skip and Seek move the read position", "[ByteReader]")
{
    const auto buffer = MakeBuffer({0x00, 0x11, 0x22, 0x33, 0x44, 0x55});
    ByteReader reader(buffer);

    SECTION("Skip advances relative to the current offset")
    {
        REQUIRE(reader.Skip(2));
        CHECK(reader.ReadU8() == uint8_t{0x22});
        REQUIRE(reader.Skip(2));
        CHECK(reader.ReadU8() == uint8_t{0x55});
    }

    SECTION("Seek moves to an absolute offset, including backwards")
    {
        REQUIRE(reader.Seek(4));
        CHECK(reader.ReadU8() == uint8_t{0x44});
        REQUIRE(reader.Seek(1));
        CHECK(reader.ReadU16() == uint16_t{0x2211});
    }

    SECTION("Seeking to the end is allowed and leaves nothing to read")
    {
        REQUIRE(reader.Seek(buffer.size()));
        CHECK(reader.Remaining() == 0);
        CHECK_FALSE(reader.ReadU8().has_value());
    }
}

TEST_CASE("Reads that end exactly at the buffer end succeed", "[ByteReader]")
{
    const auto buffer = MakeBuffer({0x01, 0x02, 0x03, 0x04, 0x05, 0x06});
    ByteReader reader(buffer);

    SECTION("Integer read consumes the last bytes")
    {
        REQUIRE(reader.Skip(2));
        CHECK(reader.ReadU32() == uint32_t{0x06050403});
        CHECK(reader.Remaining() == 0);
        CHECK_FALSE(reader.ReadU8().has_value());
    }

    SECTION("ReadBytes of everything remaining")
    {
        const auto bytes = reader.ReadBytes(buffer.size());
        REQUIRE(bytes.has_value());
        CHECK(bytes->size() == buffer.size());
        CHECK(reader.Remaining() == 0);
    }

    SECTION("Skip to the end, then zero-length operations still succeed")
    {
        REQUIRE(reader.Skip(buffer.size()));
        CHECK(reader.Skip(0));
        const auto empty = reader.ReadBytes(0);
        REQUIRE(empty.has_value());
        CHECK(empty->empty());
    }
}

TEST_CASE("Out-of-bounds reads fail without moving", "[ByteReader]")
{
    const auto buffer = MakeBuffer({0xAA, 0xBB, 0xCC});
    ByteReader reader(buffer);

    SECTION("Integer reads larger than the buffer")
    {
        CHECK_FALSE(reader.ReadU32().has_value());
        CHECK_FALSE(reader.ReadU64().has_value());
        CHECK(reader.Offset() == 0);
    }

    SECTION("A partial read fails, then smaller reads still work")
    {
        REQUIRE(reader.ReadU16() == uint16_t{0xBBAA});
        CHECK_FALSE(reader.ReadU16().has_value());
        CHECK(reader.Offset() == 2);
        CHECK(reader.ReadU8() == uint8_t{0xCC});
        CHECK_FALSE(reader.ReadU8().has_value());
        CHECK(reader.Offset() == 3);
    }

    SECTION("ReadBytes, Skip and Seek past the end")
    {
        REQUIRE(reader.Skip(1));
        CHECK_FALSE(reader.ReadBytes(3).has_value());
        CHECK_FALSE(reader.Skip(3));
        CHECK_FALSE(reader.Seek(4));
        CHECK(reader.Offset() == 1);
    }

    SECTION("Huge counts do not overflow the bounds check")
    {
        constexpr size_t kHuge = std::numeric_limits<size_t>::max();
        REQUIRE(reader.Skip(1));
        CHECK_FALSE(reader.ReadBytes(kHuge).has_value());
        CHECK_FALSE(reader.Skip(kHuge));
        CHECK_FALSE(reader.Seek(kHuge));
        CHECK(reader.Offset() == 1);
    }
}

TEST_CASE("ReadStruct deserializes packed ABI structs", "[ByteReader]")
{
    SECTION("Header with natural field order")
    {
        const auto buffer = MakeBuffer({
            0x64,
            0x76,
            0x78,
            0x2D, // Magic
            0x02,
            0x00, // Version
            0x01,
            0x80, // Flags
            0x00,
            0x10,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00, // ContentSize
        });
        ByteReader reader(buffer);

        const auto header = reader.ReadStruct<TestHeader>();
        REQUIRE(header.has_value());
        CHECK(header->Magic == 0x2D787664U);
        CHECK(header->Version == 2);
        CHECK(header->Flags == 0x8001);
        CHECK(header->ContentSize == 0x1000);
        CHECK(reader.Remaining() == 0);
    }

    SECTION("Consecutive odd-sized records")
    {
        const auto buffer = MakeBuffer({
            0x01,
            0x44,
            0x33,
            0x22,
            0x11, // Kind 1, Value 0x11223344
            0x02,
            0xFF,
            0x00,
            0x00,
            0x00, // Kind 2, Value 0xFF
        });
        ByteReader reader(buffer);

        const auto first = reader.ReadStruct<TestRecord>();
        const auto second = reader.ReadStruct<TestRecord>();
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->Kind == 1);
        CHECK(first->Value == 0x11223344U);
        CHECK(second->Kind == 2);
        CHECK(second->Value == 0xFFU);
        CHECK_FALSE(reader.ReadStruct<TestRecord>().has_value());
    }

    SECTION("Enums can be read directly")
    {
        const auto buffer = MakeBuffer({0x02, 0x01});
        ByteReader reader(buffer);

        CHECK(reader.ReadStruct<TestKind>() == TestKind::Package);
    }

    SECTION("A struct larger than the remaining bytes fails without moving")
    {
        const auto buffer = MakeBuffer({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A});
        ByteReader reader(buffer);

        REQUIRE(reader.Skip(1));
        CHECK_FALSE(reader.ReadStruct<TestHeader>().has_value());
        CHECK(reader.Offset() == 1);

        // The remaining 9 bytes still hold one full record.
        const auto record = reader.ReadStruct<TestRecord>();
        REQUIRE(record.has_value());
        CHECK(record->Kind == 0x02);
        CHECK(record->Value == 0x06050403U);
    }
}
