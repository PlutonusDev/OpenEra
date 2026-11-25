#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"
#include "Loader/ImageMapper.h"
#include "Loader/ImageMemory.h"
#include "Loader/PeReader.h"

using namespace OpenEra::Loader;
using OpenEra::Core::Logger;
using OpenEra::Core::LogRecord;
using OpenEra::Core::LogSink;

namespace
{
// Synthetic image: headers 0x400 bytes, then
//   .text  RVA 0x1000, 0x100 in memory, 0x200 raw at 0x400 (raw tail must not be copied), execute/read
//   .data  RVA 0x2000, 0x800 in memory, 0x200 raw at 0x600 (zero-filled tail), read/write,
//          with a pointer to .text at RVA 0x2010 that needs relocating
//   .rdata RVA 0x3000, 0x80 in memory, 0x200 raw at 0x800, read-only
constexpr uint64_t kPreferredBase = 0x1'4000'0000ULL;
constexpr uint32_t kSizeOfImage = 0x4000;
constexpr uint32_t kSizeOfHeaders = 0x400;
constexpr uint32_t kPointerRva = 0x2010;
constexpr size_t kFileSize = 0xA00;

constexpr std::byte kHeaderFill{0x11};
constexpr std::byte kTextFill{0xC3};
constexpr std::byte kDataFill{0xDD};
constexpr std::byte kRdataFill{0xAA};

SectionHeader MakeSection(std::string_view name, uint32_t rva, uint32_t virtualSize, uint32_t rawOffset,
                          uint32_t characteristics)
{
    SectionHeader section{};
    std::ranges::copy(name.substr(0, section.Name.size()), section.Name.begin());
    section.VirtualAddress = rva;
    section.VirtualSize = virtualSize;
    section.SizeOfRawData = 0x200;
    section.PointerToRawData = rawOffset;
    section.Characteristics = characteristics;
    return section;
}

uint64_t ReadU64(std::span<const std::byte> bytes, size_t offset)
{
    std::array<std::byte, sizeof(uint64_t)> raw{};
    std::ranges::copy(bytes.subspan(offset, raw.size()), raw.begin());
    return std::bit_cast<uint64_t>(raw);
}

void WriteU64(std::span<std::byte> bytes, size_t offset, uint64_t value)
{
    std::ranges::copy(std::bit_cast<std::array<std::byte, sizeof(uint64_t)>>(value), bytes.subspan(offset).begin());
}

bool AllEqual(std::span<const std::byte> bytes, std::byte value)
{
    return std::ranges::all_of(bytes, [value](std::byte byte) { return byte == value; });
}

struct TestImage
{
    PeImage Image;
    std::vector<std::byte> File = std::vector<std::byte>(kFileSize);

    TestImage()
    {
        Image.FileHeader.Characteristics = 0x0022;
        Image.OptionalHeader.Magic = kOptionalHeaderMagicPe32Plus;
        Image.OptionalHeader.ImageBase = kPreferredBase;
        Image.OptionalHeader.SizeOfImage = kSizeOfImage;
        Image.OptionalHeader.SizeOfHeaders = kSizeOfHeaders;
        Image.OptionalHeader.SectionAlignment = 0x1000;
        Image.OptionalHeader.FileAlignment = 0x200;
        Image.Sections = {
            MakeSection(".text", 0x1000, 0x100, 0x400, kSectionMemoryExecute | kSectionMemoryRead | 0x20),
            MakeSection(".data", 0x2000, 0x800, 0x600, kSectionMemoryRead | kSectionMemoryWrite | 0x40),
            MakeSection(".rdata", 0x3000, 0x80, 0x800, kSectionMemoryRead | 0x40),
        };
        Image.RelocationRvas = {kPointerRva};

        const auto file = std::span(File);
        std::ranges::fill(file.subspan(0, kSizeOfHeaders), kHeaderFill);
        std::ranges::fill(file.subspan(0x400, 0x200), kTextFill);
        std::ranges::fill(file.subspan(0x600, 0x200), kDataFill);
        std::ranges::fill(file.subspan(0x800, 0x200), kRdataFill);
        WriteU64(file, 0x610, kPreferredBase + 0x1000);
    }
};

struct ProtectCall
{
    size_t Offset = 0;
    size_t Size = 0;
    PageProtection Protection = PageProtection::NoAccess;

    bool operator==(const ProtectCall&) const = default;
};

class FakeMemory final : public ImageMemory
{
public:
    FakeMemory(uint64_t base, size_t size, std::vector<ProtectCall>* calls, bool failProtect)
        : m_base(base)
        , m_bytes(size)
        , m_calls(calls)
        , m_failProtect(failProtect)
    {
    }

    [[nodiscard]] uint64_t Base() const noexcept override { return m_base; }
    [[nodiscard]] std::span<std::byte> Bytes() noexcept override { return m_bytes; }

    [[nodiscard]] bool Protect(size_t offset, size_t size, PageProtection protection) noexcept override
    {
        try
        {
            m_calls->push_back({.Offset = offset, .Size = size, .Protection = protection});
        }
        catch (...)
        {
            return false;
        }
        return !m_failProtect;
    }

private:
    uint64_t m_base;
    std::vector<std::byte> m_bytes;
    std::vector<ProtectCall>* m_calls;
    bool m_failProtect;
};

class FakeAllocator final : public ImageAllocator
{
public:
    std::optional<uint64_t> ForcedBase;
    bool Fail = false;
    bool FailProtect = false;
    std::vector<ProtectCall> Calls;
    uint64_t RequestedBase = 0;
    size_t RequestedSize = 0;

    [[nodiscard]] std::unique_ptr<ImageMemory> Allocate(uint64_t preferredBase, size_t size) noexcept override
    {
        RequestedBase = preferredBase;
        RequestedSize = size;
        if (Fail)
        {
            return nullptr;
        }
        try
        {
            return std::make_unique<FakeMemory>(ForcedBase.value_or(preferredBase), size, &Calls, FailProtect);
        }
        catch (...)
        {
            return nullptr;
        }
    }
};

class CaptureSink final : public LogSink
{
public:
    void Write(const LogRecord& record) override { m_messages.emplace_back(record.Message); }

    [[nodiscard]] bool Contains(std::string_view text) const
    {
        return std::ranges::any_of(m_messages, [text](const std::string& message)
                                   { return message.find(text) != std::string::npos; });
    }

private:
    std::vector<std::string> m_messages;
};

struct LoggedMap
{
    Logger Log;
    std::shared_ptr<CaptureSink> Sink = std::make_shared<CaptureSink>();

    LoggedMap() { Log.AddSink(Sink); }

    MapResult operator()(const TestImage& test, ImageAllocator& allocator)
    {
        return MapImage(test.File, test.Image, allocator, Log);
    }
};
} // namespace

TEST_CASE("SectionProtection maps section flags to page protections", "[ImageMapper]")
{
    CHECK(SectionProtection(0) == PageProtection::NoAccess);
    CHECK(SectionProtection(kSectionMemoryRead) == PageProtection::ReadOnly);
    CHECK(SectionProtection(kSectionMemoryWrite) == PageProtection::ReadWrite);
    CHECK(SectionProtection(kSectionMemoryRead | kSectionMemoryWrite) == PageProtection::ReadWrite);
    CHECK(SectionProtection(kSectionMemoryExecute) == PageProtection::Execute);
    CHECK(SectionProtection(kSectionMemoryExecute | kSectionMemoryRead) == PageProtection::ExecuteRead);
    CHECK(SectionProtection(kSectionMemoryExecute | kSectionMemoryWrite) == PageProtection::ExecuteReadWrite);
    CHECK(SectionProtection(kSectionMemoryExecute | kSectionMemoryRead | kSectionMemoryWrite) ==
          PageProtection::ExecuteReadWrite);
}

TEST_CASE("MapImage lays out headers and sections at the preferred base", "[ImageMapper]")
{
    const TestImage test;
    FakeAllocator allocator;
    LoggedMap map;
    const auto result = map(test, allocator);
    REQUIRE(result.Error == MapError::None);
    REQUIRE(result.Image.has_value());

    CHECK(allocator.RequestedBase == kPreferredBase);
    CHECK(allocator.RequestedSize == kSizeOfImage);
    CHECK(result.Image->Base() == kPreferredBase);
    CHECK_FALSE(result.Image->Relocated());

    const auto memory = std::span<const std::byte>(result.Image->Memory->Bytes());
    REQUIRE(memory.size() == kSizeOfImage);
    CHECK(AllEqual(memory.subspan(0, kSizeOfHeaders), kHeaderFill));
    CHECK(AllEqual(memory.subspan(kSizeOfHeaders, 0x1000 - kSizeOfHeaders), std::byte{0}));

    // .text: only VirtualSize bytes are copied, even though more raw data exists.
    CHECK(AllEqual(memory.subspan(0x1000, 0x100), kTextFill));
    CHECK(AllEqual(memory.subspan(0x1100, 0xF00), std::byte{0}));

    // .data: raw data, then a zero-filled tail up to VirtualSize.
    CHECK(AllEqual(memory.subspan(0x2000, 0x10), kDataFill));
    CHECK(ReadU64(memory, kPointerRva) == kPreferredBase + 0x1000); // Unchanged at the preferred base.
    CHECK(AllEqual(memory.subspan(0x2018, 0x1E8), kDataFill));
    CHECK(AllEqual(memory.subspan(0x2200, 0x600), std::byte{0}));

    CHECK(AllEqual(memory.subspan(0x3000, 0x80), kRdataFill));
    CHECK(AllEqual(memory.subspan(0x3080, 0xF80), std::byte{0}));

    CHECK(allocator.Calls == std::vector<ProtectCall>{
                                 {.Offset = 0, .Size = 0x1000, .Protection = PageProtection::ReadOnly},
                                 {.Offset = 0x1000, .Size = 0x1000, .Protection = PageProtection::ExecuteRead},
                                 {.Offset = 0x2000, .Size = 0x1000, .Protection = PageProtection::ReadWrite},
                                 {.Offset = 0x3000, .Size = 0x1000, .Protection = PageProtection::ReadOnly},
                             });
}

TEST_CASE("MapImage applies base relocations away from the preferred base", "[ImageMapper]")
{
    const TestImage test;
    FakeAllocator allocator;

    SECTION("higher base")
    {
        allocator.ForcedBase = 0x2'0000'0000ULL;
    }

    SECTION("lower base")
    {
        allocator.ForcedBase = 0x1000'0000ULL;
    }

    const auto result = MapImage(test.File, test.Image, allocator);
    REQUIRE(result.Error == MapError::None);
    CHECK(result.Image->Relocated());
    CHECK(result.Image->PreferredBase == kPreferredBase);

    const auto memory = std::span<const std::byte>(result.Image->Memory->Bytes());
    CHECK(ReadU64(memory, kPointerRva) == *allocator.ForcedBase + 0x1000);
    CHECK(AllEqual(memory.subspan(0x2000, 0x10), kDataFill)); // Only the relocated field changes.
}

TEST_CASE("MapImage handles images without relocations", "[ImageMapper]")
{
    TestImage test;
    test.Image.FileHeader.Characteristics |= kFileRelocationsStripped;
    FakeAllocator allocator;
    LoggedMap map;

    SECTION("at the preferred base it maps normally")
    {
        CHECK(map(test, allocator).Error == MapError::None);
    }

    SECTION("anywhere else it fails")
    {
        allocator.ForcedBase = 0x2'0000'0000ULL;
        CHECK(map(test, allocator).Error == MapError::RelocationsStripped);
        CHECK(map.Sink->Contains("no relocations"));
    }
}

TEST_CASE("MapImage rejects bad layouts before allocating", "[ImageMapper]")
{
    TestImage test;
    FakeAllocator allocator;
    LoggedMap map;
    MapError expected = MapError::BadImageLayout;

    SECTION("headers larger than the image")
    {
        test.Image.OptionalHeader.SizeOfHeaders = kSizeOfImage + 1;
    }

    SECTION("empty image")
    {
        test.Image.OptionalHeader.SizeOfImage = 0;
        test.Image.OptionalHeader.SizeOfHeaders = 0;
    }

    SECTION("section past SizeOfImage")
    {
        test.Image.Sections.at(2).VirtualSize = 0x1001;
    }

    SECTION("section overlapping the headers")
    {
        test.Image.Sections.at(0).VirtualAddress = kSizeOfHeaders - 1;
    }

    SECTION("overlapping sections")
    {
        test.Image.Sections.at(1).VirtualSize = 0x1001;
    }

    SECTION("raw data past the end of the file")
    {
        test.Image.Sections.at(2).PointerToRawData = kFileSize - 0x7F;
        expected = MapError::TruncatedSection;
    }

    CHECK(map(test, allocator).Error == expected);
    CHECK(allocator.RequestedSize == 0);
}

TEST_CASE("MapImage reports allocation, relocation and protection failures", "[ImageMapper]")
{
    TestImage test;
    FakeAllocator allocator;
    LoggedMap map;

    SECTION("allocation failure")
    {
        allocator.Fail = true;
        CHECK(map(test, allocator).Error == MapError::AllocationFailed);
        CHECK(map.Sink->Contains("could not allocate 0x4000 bytes"));
    }

    SECTION("relocation outside the image")
    {
        allocator.ForcedBase = 0x2'0000'0000ULL;
        test.Image.RelocationRvas.push_back(kSizeOfImage - 4);
        CHECK(map(test, allocator).Error == MapError::BadRelocation);
        CHECK(map.Sink->Contains("relocation at RVA 0x3ffc"));
    }

    SECTION("protection failure")
    {
        allocator.FailProtect = true;
        CHECK(map(test, allocator).Error == MapError::ProtectionFailed);
        CHECK(map.Sink->Contains("failed to protect the headers"));
    }
}

TEST_CASE("MapImage warns about weak protections", "[ImageMapper]")
{
    TestImage test;
    FakeAllocator allocator;
    LoggedMap map;

    SECTION("writable and executable section")
    {
        test.Image.Sections.at(0).Characteristics |= kSectionMemoryWrite;
        REQUIRE(map(test, allocator).Error == MapError::None);
        CHECK(map.Sink->Contains("section 0 is writable and executable"));
        CHECK(allocator.Calls.at(1).Protection == PageProtection::ExecuteReadWrite);
    }

    SECTION("section alignment below the page size")
    {
        test.Image.OptionalHeader.SectionAlignment = 0x200;
        REQUIRE(map(test, allocator).Error == MapError::None);
        CHECK(map.Sink->Contains("below the page size"));
        CHECK(allocator.Calls ==
              std::vector<ProtectCall>{
                  {.Offset = 0, .Size = kSizeOfImage, .Protection = PageProtection::ExecuteReadWrite}});
    }
}

TEST_CASE("MapImage works with VirtualAlloc memory", "[ImageMapper]")
{
    const TestImage test;
    const auto allocator = CreateVirtualImageAllocator();
    REQUIRE(allocator != nullptr);

    SECTION("wherever the image lands, its pointer matches its base")
    {
        const auto result = MapImage(test.File, test.Image, *allocator);
        REQUIRE(result.Error == MapError::None);
        const auto memory = std::span<const std::byte>(result.Image->Memory->Bytes());
        CHECK(ReadU64(memory, kPointerRva) == result.Image->Base() + 0x1000);
        CHECK(AllEqual(memory.subspan(0x1000, 0x100), kTextFill));
        CHECK(AllEqual(memory.subspan(0x2200, 0x600), std::byte{0}));
    }

    SECTION("an occupied preferred base forces relocation")
    {
        const auto blocker = allocator->Allocate(kPreferredBase, 0x10000);
        REQUIRE(blocker != nullptr);
        const auto result = MapImage(test.File, test.Image, *allocator);
        REQUIRE(result.Error == MapError::None);
        CHECK(result.Image->Base() != blocker->Base());
        if (blocker->Base() == kPreferredBase)
        {
            CHECK(result.Image->Relocated());
        }
        CHECK(ReadU64(result.Image->Memory->Bytes(), kPointerRva) == result.Image->Base() + 0x1000);
    }

    SECTION("Protect rejects ranges outside the region")
    {
        const auto memory = allocator->Allocate(0, 0x2000);
        REQUIRE(memory != nullptr);
        CHECK(memory->Protect(0, 0x2000, PageProtection::ReadOnly));
        CHECK_FALSE(memory->Protect(0x1000, 0x1001, PageProtection::ReadOnly));
        CHECK_FALSE(memory->Protect(0x2001, 0, PageProtection::ReadOnly));
    }

    SECTION("zero-sized allocations fail")
    {
        CHECK(allocator->Allocate(0, 0) == nullptr);
    }
}

TEST_CASE("Mapper enum names", "[ImageMapper]")
{
    CHECK(ToString(MapError::RelocationsStripped) == "RelocationsStripped");
    CHECK(ToString(PageProtection::ExecuteRead) == "ExecuteRead");
}
