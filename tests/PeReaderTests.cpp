#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Loader/PeReader.h"

using namespace OpenEra::Loader;

namespace
{
// Layout of the synthetic image (file offsets unless noted):
//   0x000 DOS header           -> PE header at 0x40
//   0x040 "PE\0\0"
//   0x044 COFF file header
//   0x058 PE32+ optional header (112 bytes) + 16 data directories
//   0x148 section table (2 x 40 bytes)
//   0x200 .text  raw data (RVA 0x1000, 0x200 bytes)
//   0x400 .idata raw data (RVA 0x2000, 0x200 bytes on disk, 0x300 in memory)
constexpr size_t kPeHeaderOffset = 0x40;
constexpr size_t kFileHeaderOffset = 0x44;
constexpr size_t kOptionalHeaderOffset = 0x58;
constexpr size_t kDataDirectoryOffset = kOptionalHeaderOffset + sizeof(OptionalHeader64);
constexpr size_t kSectionTableOffset = 0x148;
constexpr uint16_t kOptionalHeaderSize = sizeof(OptionalHeader64) + (kMaxDataDirectories * sizeof(DataDirectory));
constexpr size_t kFileSize = 0x600;

constexpr uint32_t kTextRva = 0x1000;
constexpr uint32_t kTextFileOffset = 0x200;
constexpr uint32_t kIdataRva = 0x2000;
constexpr uint32_t kIdataFileOffset = 0x400;
constexpr uint32_t kIdataRawSize = 0x200;

// Import data inside .idata (RVAs).
constexpr uint32_t kDescriptorsRva = 0x2000;
constexpr uint32_t kKernelLookupRva = 0x2080;
constexpr uint32_t kKernelAddressRva = 0x20C0;
constexpr uint32_t kUserAddressRva = 0x20E0;
constexpr uint32_t kKernelNameRva = 0x2100;
constexpr uint32_t kUserNameRva = 0x2110;
constexpr uint32_t kCreateFileRva = 0x2120;
constexpr uint32_t kMessageBoxRva = 0x2140;

constexpr size_t IdataFileOffset(uint32_t rva) noexcept
{
    return size_t{rva} - kIdataRva + kIdataFileOffset;
}

constexpr std::array<char, 8> MakeSectionName(std::string_view name) noexcept
{
    std::array<char, 8> result{};
    std::ranges::copy(name.substr(0, result.size()), result.begin());
    return result;
}

template <typename T>
void Put(std::vector<std::byte>& bytes, size_t offset, const T& value)
{
    const auto raw = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    if (offset > bytes.size() || bytes.size() - offset < raw.size())
    {
        throw std::out_of_range("synthetic PE write out of range");
    }
    std::ranges::copy(raw, std::span(bytes).subspan(offset).begin());
}

void PutString(std::vector<std::byte>& bytes, size_t offset, std::string_view text)
{
    for (const char character : text)
    {
        Put(bytes, offset++, character);
    }
    Put(bytes, offset, '\0');
}

// Builds a minimal, valid PE32+ image. Tests adjust the public fields before
// calling Build(), or patch the returned bytes afterwards.
struct ImageBuilder
{
    DosHeader Dos{.Signature = kDosSignature, .Stub = {}, .PeHeaderOffset = kPeHeaderOffset};
    uint32_t Signature = kPeSignature;
    CoffFileHeader FileHeader{.Machine = kMachineAmd64,
                              .SectionCount = 2,
                              .TimeDateStamp = 0x6543'2100,
                              .SymbolTableOffset = 0,
                              .SymbolCount = 0,
                              .OptionalHeaderSize = kOptionalHeaderSize,
                              .Characteristics = 0x0022}; // Executable, large-address aware.
    OptionalHeader64 Optional{};
    std::array<DataDirectory, kMaxDataDirectories> Directories{};
    std::vector<SectionHeader> Sections;
    std::array<ImportDescriptor, 3> Descriptors{};
    std::array<uint64_t, 3> KernelLookup{};
    std::array<uint64_t, 2> UserAddress{};

    ImageBuilder()
    {
        Optional.Magic = kOptionalHeaderMagicPe32Plus;
        Optional.MajorLinkerVersion = 14;
        Optional.AddressOfEntryPoint = kTextRva;
        Optional.BaseOfCode = kTextRva;
        Optional.ImageBase = 0x1'4000'0000ULL;
        Optional.SectionAlignment = 0x1000;
        Optional.FileAlignment = 0x200;
        Optional.SizeOfImage = 0x3000;
        Optional.SizeOfHeaders = 0x200;
        Optional.Subsystem = 2; // Windows GUI
        Optional.NumberOfRvaAndSizes = kMaxDataDirectories;

        Directories.at(static_cast<size_t>(DataDirectoryIndex::Import)) = {
            .VirtualAddress = kDescriptorsRva, .Size = 60}; // Two descriptors plus the null terminator.

        Sections.push_back({.Name = MakeSectionName(".text"),
                            .VirtualSize = 0x10,
                            .VirtualAddress = kTextRva,
                            .SizeOfRawData = 0x200,
                            .PointerToRawData = kTextFileOffset,
                            .PointerToRelocations = 0,
                            .PointerToLineNumbers = 0,
                            .RelocationCount = 0,
                            .LineNumberCount = 0,
                            .Characteristics = 0x6000'0020}); // Code, execute, read.
        Sections.push_back({.Name = MakeSectionName(".idata"),
                            .VirtualSize = 0x300,
                            .VirtualAddress = kIdataRva,
                            .SizeOfRawData = kIdataRawSize,
                            .PointerToRawData = kIdataFileOffset,
                            .PointerToRelocations = 0,
                            .PointerToLineNumbers = 0,
                            .RelocationCount = 0,
                            .LineNumberCount = 0,
                            .Characteristics = 0xC000'0040}); // Initialized data, read, write.

        // KERNEL32.dll: by-name and by-ordinal imports through its lookup table.
        Descriptors.at(0) = {.ImportLookupTableRva = kKernelLookupRva,
                             .TimeDateStamp = 0,
                             .ForwarderChain = 0,
                             .NameRva = kKernelNameRva,
                             .ImportAddressTableRva = kKernelAddressRva};
        // USER32.dll: no lookup table, so the reader falls back to the address table.
        Descriptors.at(1) = {.ImportLookupTableRva = 0,
                             .TimeDateStamp = 0,
                             .ForwarderChain = 0,
                             .NameRva = kUserNameRva,
                             .ImportAddressTableRva = kUserAddressRva};
        KernelLookup = {kCreateFileRva, kImportByOrdinalFlag | 42U, 0};
        UserAddress = {kMessageBoxRva, 0};
    }

    [[nodiscard]] std::vector<std::byte> Build() const
    {
        std::vector<std::byte> bytes(kFileSize);
        Put(bytes, 0, Dos);
        Put(bytes, kPeHeaderOffset, Signature);
        Put(bytes, kFileHeaderOffset, FileHeader);
        Put(bytes, kOptionalHeaderOffset, Optional);
        Put(bytes, kDataDirectoryOffset, Directories);

        size_t sectionOffset = kSectionTableOffset;
        for (const auto& section : Sections)
        {
            Put(bytes, sectionOffset, section);
            sectionOffset += sizeof(SectionHeader);
        }

        Put(bytes, IdataFileOffset(kDescriptorsRva), Descriptors);
        Put(bytes, IdataFileOffset(kKernelLookupRva), KernelLookup);
        Put(bytes, IdataFileOffset(kKernelAddressRva), KernelLookup); // Unbound: IAT mirrors the ILT.
        Put(bytes, IdataFileOffset(kUserAddressRva), UserAddress);
        PutString(bytes, IdataFileOffset(kKernelNameRva), "KERNEL32.dll");
        PutString(bytes, IdataFileOffset(kUserNameRva), "USER32.dll");
        Put(bytes, IdataFileOffset(kCreateFileRva), uint16_t{5});
        PutString(bytes, IdataFileOffset(kCreateFileRva) + 2, "CreateFileW");
        Put(bytes, IdataFileOffset(kMessageBoxRva), uint16_t{7});
        PutString(bytes, IdataFileOffset(kMessageBoxRva) + 2, "MessageBoxW");
        return bytes;
    }
};

PeParseResult Parse(const std::vector<std::byte>& bytes) noexcept
{
    return ParsePe(std::span(bytes));
}

PeError ParseError(const ImageBuilder& builder)
{
    return Parse(builder.Build()).Error;
}
} // namespace

TEST_CASE("PE structures match the documented layout", "[PeReader]")
{
    STATIC_REQUIRE(sizeof(DosHeader) == 64);
    STATIC_REQUIRE(sizeof(CoffFileHeader) == 20);
    STATIC_REQUIRE(sizeof(DataDirectory) == 8);
    STATIC_REQUIRE(sizeof(OptionalHeader64) == 112);
    STATIC_REQUIRE(sizeof(SectionHeader) == 40);
    STATIC_REQUIRE(sizeof(ImportDescriptor) == 20);

    STATIC_REQUIRE(offsetof(DosHeader, PeHeaderOffset) == 0x3C);
    STATIC_REQUIRE(offsetof(CoffFileHeader, OptionalHeaderSize) == 16);
    STATIC_REQUIRE(offsetof(OptionalHeader64, AddressOfEntryPoint) == 16);
    STATIC_REQUIRE(offsetof(OptionalHeader64, SizeOfHeaders) == 60);
    STATIC_REQUIRE(offsetof(OptionalHeader64, Subsystem) == 68);
    STATIC_REQUIRE(offsetof(OptionalHeader64, NumberOfRvaAndSizes) == 108);
    STATIC_REQUIRE(offsetof(SectionHeader, VirtualSize) == 8);
    STATIC_REQUIRE(offsetof(SectionHeader, PointerToRawData) == 20);
    STATIC_REQUIRE(offsetof(SectionHeader, Characteristics) == 36);
    STATIC_REQUIRE(offsetof(ImportDescriptor, NameRva) == 12);
    STATIC_REQUIRE(offsetof(ImportDescriptor, ImportAddressTableRva) == 16);
}

TEST_CASE("ParsePe reads the headers and section table", "[PeReader]")
{
    const auto bytes = ImageBuilder{}.Build();
    const auto result = Parse(bytes);
    REQUIRE(result.Error == PeError::None);
    REQUIRE(result.Image.has_value());
    const auto& image = result.Image.value();

    CHECK(image.FileHeader.Machine == kMachineAmd64);
    CHECK(image.FileHeader.SectionCount == 2);
    CHECK(image.FileHeader.TimeDateStamp == 0x6543'2100);
    CHECK(image.FileHeader.OptionalHeaderSize == kOptionalHeaderSize);

    CHECK(image.OptionalHeader.Magic == kOptionalHeaderMagicPe32Plus);
    CHECK(image.OptionalHeader.AddressOfEntryPoint == kTextRva);
    CHECK(image.OptionalHeader.ImageBase == 0x1'4000'0000ULL);
    CHECK(image.OptionalHeader.SectionAlignment == 0x1000);
    CHECK(image.OptionalHeader.FileAlignment == 0x200);
    CHECK(image.OptionalHeader.SizeOfImage == 0x3000);
    CHECK(image.OptionalHeader.Subsystem == 2);

    REQUIRE(image.DataDirectories.size() == kMaxDataDirectories);
    const auto import = image.Directory(DataDirectoryIndex::Import);
    REQUIRE(import.has_value());
    CHECK(import->VirtualAddress == kDescriptorsRva);
    CHECK(import->Size == 60);
    const auto exports = image.Directory(DataDirectoryIndex::Export);
    REQUIRE(exports.has_value());
    CHECK(exports->VirtualAddress == 0);

    REQUIRE(image.Sections.size() == 2);
    const auto& text = image.Sections.at(0);
    CHECK(SectionName(text) == ".text");
    CHECK(text.VirtualAddress == kTextRva);
    CHECK(text.PointerToRawData == kTextFileOffset);
    CHECK(text.Characteristics == 0x6000'0020);
    const auto& idata = image.Sections.at(1);
    CHECK(SectionName(idata) == ".idata");
    CHECK(idata.VirtualSize == 0x300);
    CHECK(idata.SizeOfRawData == kIdataRawSize);
}

TEST_CASE("ParsePe reads import descriptors, by name and by ordinal", "[PeReader]")
{
    const auto bytes = ImageBuilder{}.Build();
    const auto result = Parse(bytes);
    REQUIRE(result.Error == PeError::None);
    const auto& imports = result.Image.value().Imports;
    REQUIRE(imports.size() == 2);

    const auto& kernel = imports.at(0);
    CHECK(kernel.DllName == "KERNEL32.dll");
    CHECK(kernel.ImportAddressTableRva == kKernelAddressRva);
    REQUIRE(kernel.Entries.size() == 2);
    CHECK_FALSE(kernel.Entries.at(0).Ordinal.has_value());
    CHECK(kernel.Entries.at(0).Hint == 5);
    CHECK(kernel.Entries.at(0).Name == "CreateFileW");
    REQUIRE(kernel.Entries.at(1).Ordinal.has_value());
    CHECK(kernel.Entries.at(1).Ordinal.value() == 42);
    CHECK(kernel.Entries.at(1).Name.empty());

    // No lookup table: entries come from the address table instead.
    const auto& user = imports.at(1);
    CHECK(user.DllName == "USER32.dll");
    CHECK(user.ImportAddressTableRva == kUserAddressRva);
    REQUIRE(user.Entries.size() == 1);
    CHECK(user.Entries.at(0).Hint == 7);
    CHECK(user.Entries.at(0).Name == "MessageBoxW");
}

TEST_CASE("ParsePe handles images without imports", "[PeReader]")
{
    ImageBuilder builder;

    SECTION("empty import directory")
    {
        builder.Directories.at(static_cast<size_t>(DataDirectoryIndex::Import)) = {};
    }

    SECTION("fewer data directories than the import index")
    {
        builder.Optional.NumberOfRvaAndSizes = 1;
    }

    SECTION("no data directories, smallest valid optional header")
    {
        builder.Optional.NumberOfRvaAndSizes = 0;
        builder.FileHeader.OptionalHeaderSize = sizeof(OptionalHeader64);
        // The section table moves up to directly after the fixed fields.
        auto bytes = builder.Build();
        const auto shifted = std::span(bytes).subspan(kSectionTableOffset, 2 * sizeof(SectionHeader));
        std::ranges::copy(shifted, std::span(bytes).subspan(kDataDirectoryOffset).begin());

        const auto result = Parse(bytes);
        REQUIRE(result.Error == PeError::None);
        CHECK(result.Image->DataDirectories.empty());
        CHECK(SectionName(result.Image->Sections.at(1)) == ".idata");
        CHECK(result.Image->Imports.empty());
        return;
    }

    const auto result = Parse(builder.Build());
    REQUIRE(result.Error == PeError::None);
    CHECK(result.Image->Imports.empty());
    CHECK(result.Image->Sections.size() == 2);
}

TEST_CASE("RvaToFileOffset maps through headers and sections", "[PeReader]")
{
    const auto result = Parse(ImageBuilder{}.Build());
    REQUIRE(result.Image.has_value());
    const auto& image = result.Image.value();

    CHECK(RvaToFileOffset(image, 0) == 0u);
    CHECK(RvaToFileOffset(image, 0x40) == 0x40u);
    CHECK(RvaToFileOffset(image, 0x1FF) == 0x1FFu); // Last header byte.
    CHECK(RvaToFileOffset(image, kTextRva) == kTextFileOffset);
    CHECK(RvaToFileOffset(image, kTextRva + 0xF) == kTextFileOffset + 0xFu);
    CHECK(RvaToFileOffset(image, kIdataRva + 0x123) == kIdataFileOffset + 0x123u);
    CHECK(RvaToFileOffset(image, kIdataRva + kIdataRawSize - 1) == kIdataFileOffset + kIdataRawSize - 1);

    CHECK_FALSE(RvaToFileOffset(image, 0x200).has_value());             // Gap between headers and .text.
    CHECK_FALSE(RvaToFileOffset(image, kTextRva + 0x10).has_value());   // Past .text's VirtualSize.
    CHECK_FALSE(RvaToFileOffset(image, kIdataRva + 0x250).has_value()); // .idata zero-filled tail.
    CHECK_FALSE(RvaToFileOffset(image, 0xFFFF'FFFF).has_value());
}

TEST_CASE("SectionName trims NUL padding", "[PeReader]")
{
    SectionHeader section{};
    CHECK(SectionName(section).empty());

    section.Name = MakeSectionName(".rdata");
    CHECK(SectionName(section) == ".rdata");

    section.Name = MakeSectionName("12345678"); // All eight bytes used, no terminator.
    CHECK(SectionName(section) == "12345678");
}

TEST_CASE("ParsePe rejects malformed headers", "[PeReader]")
{
    ImageBuilder builder;

    SECTION("empty and short buffers")
    {
        CHECK(ParsePe({}).Error == PeError::TruncatedDosHeader);
        auto bytes = builder.Build();
        bytes.resize(sizeof(DosHeader) - 1);
        CHECK(Parse(bytes).Error == PeError::TruncatedDosHeader);
    }

    SECTION("bad DOS signature")
    {
        builder.Dos.Signature = 0x4D5A; // "ZM"
        CHECK(ParseError(builder) == PeError::BadDosSignature);
    }

    SECTION("PE header offset past the end of the file")
    {
        builder.Dos.PeHeaderOffset = 0xFFFF'FFFF;
        CHECK(ParseError(builder) == PeError::BadPeHeaderOffset);
    }

    SECTION("PE header offset at the very end of the file")
    {
        builder.Dos.PeHeaderOffset = kFileSize;
        CHECK(ParseError(builder) == PeError::BadPeSignature);
    }

    SECTION("bad PE signature")
    {
        builder.Signature = 0x0100'4550; // "PE\0\x01"
        CHECK(ParseError(builder) == PeError::BadPeSignature);
    }

    SECTION("truncated COFF file header")
    {
        auto bytes = builder.Build();
        bytes.resize(kFileHeaderOffset + sizeof(CoffFileHeader) - 1);
        CHECK(Parse(bytes).Error == PeError::TruncatedFileHeader);
    }

    SECTION("PE32 optional header is not supported")
    {
        builder.Optional.Magic = kOptionalHeaderMagicPe32;
        CHECK(ParseError(builder) == PeError::UnsupportedOptionalHeader);
    }

    SECTION("no optional header")
    {
        builder.FileHeader.OptionalHeaderSize = 0;
        CHECK(ParseError(builder) == PeError::UnsupportedOptionalHeader);
    }

    SECTION("truncated optional header")
    {
        auto bytes = builder.Build();
        bytes.resize(kOptionalHeaderOffset + sizeof(OptionalHeader64) - 1);
        CHECK(Parse(bytes).Error == PeError::TruncatedOptionalHeader);
    }

    SECTION("truncated data directories")
    {
        auto bytes = builder.Build();
        bytes.resize(kDataDirectoryOffset + (3 * sizeof(DataDirectory)) + 4);
        CHECK(Parse(bytes).Error == PeError::TruncatedOptionalHeader);
    }

    SECTION("more than 16 data directories")
    {
        builder.Optional.NumberOfRvaAndSizes = kMaxDataDirectories + 1;
        CHECK(ParseError(builder) == PeError::BadDataDirectoryCount);
    }

    SECTION("data directories larger than the declared optional header")
    {
        builder.FileHeader.OptionalHeaderSize = sizeof(OptionalHeader64) + sizeof(DataDirectory);
        CHECK(ParseError(builder) == PeError::BadDataDirectoryCount);
    }

    SECTION("section table runs past the end of the file")
    {
        builder.FileHeader.SectionCount = 0xFFFF;
        CHECK(ParseError(builder) == PeError::TruncatedSectionTable);
    }
}

TEST_CASE("ParsePe rejects malformed import tables", "[PeReader]")
{
    ImageBuilder builder;
    auto& importDirectory = builder.Directories.at(static_cast<size_t>(DataDirectoryIndex::Import));

    SECTION("import directory outside every section")
    {
        importDirectory.VirtualAddress = 0x9000;
        CHECK(ParseError(builder) == PeError::BadImportDirectory);
    }

    SECTION("import directory in a zero-filled tail")
    {
        importDirectory.VirtualAddress = kIdataRva + kIdataRawSize;
        CHECK(ParseError(builder) == PeError::BadImportDirectory);
    }

    SECTION("DLL name RVA unmapped")
    {
        builder.Descriptors.at(0).NameRva = 0x9000;
        CHECK(ParseError(builder) == PeError::BadImportName);
    }

    SECTION("empty DLL name")
    {
        builder.Descriptors.at(0).NameRva = kKernelLookupRva - 1; // A zero byte in the padding.
        CHECK(ParseError(builder) == PeError::BadImportName);
    }

    SECTION("DLL name not terminated before the end of the file")
    {
        constexpr uint32_t kNameRva = kIdataRva + kIdataRawSize - 16;
        builder.Descriptors.at(0).NameRva = kNameRva;
        auto bytes = builder.Build();
        std::ranges::fill(std::span(bytes).subspan(IdataFileOffset(kNameRva)), std::byte{'A'});
        CHECK(Parse(bytes).Error == PeError::BadImportName);
    }

    SECTION("lookup table RVA unmapped")
    {
        builder.Descriptors.at(0).ImportLookupTableRva = 0x9000;
        CHECK(ParseError(builder) == PeError::BadImportThunk);
    }

    SECTION("lookup table without a terminator")
    {
        // The table runs into the zero-filled tail, which has no file data.
        builder.Descriptors.at(0).ImportLookupTableRva = kIdataRva + kIdataRawSize - 8;
        auto bytes = builder.Build();
        Put(bytes, IdataFileOffset(kIdataRva + kIdataRawSize - 8), uint64_t{kCreateFileRva});
        CHECK(Parse(bytes).Error == PeError::BadImportThunk);
    }

    SECTION("reserved bits set in a by-name entry")
    {
        builder.KernelLookup.at(0) = kCreateFileRva | 0x0000'0001'0000'0000ULL;
        CHECK(ParseError(builder) == PeError::BadImportThunk);
    }

    SECTION("hint/name entry unmapped")
    {
        builder.KernelLookup.at(0) = 0x9000;
        CHECK(ParseError(builder) == PeError::BadImportName);
    }
}

TEST_CASE("ParsePe never reads past a truncated buffer", "[PeReader]")
{
    const auto full = ImageBuilder{}.Build();
    const auto fullSpan = std::span(full);

    for (size_t length = 0; length <= full.size(); ++length)
    {
        const auto result = ParsePe(fullSpan.first(length));
        INFO("length = " << length);
        // Image is present exactly on success.
        REQUIRE(result.Image.has_value() == (result.Error == PeError::None));
        if (length < kSectionTableOffset + (2 * sizeof(SectionHeader)))
        {
            REQUIRE(result.Error != PeError::None); // Headers are incomplete.
        }
    }

    CHECK(ParsePe(fullSpan).Error == PeError::None);
}

TEST_CASE("PeError names", "[PeReader]")
{
    CHECK(ToString(PeError::None) == "None");
    CHECK(ToString(PeError::BadPeSignature) == "BadPeSignature");
    CHECK(ToString(PeError::OutOfMemory) == "OutOfMemory");
}
