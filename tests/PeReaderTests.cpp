#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"
#include "Loader/PeReader.h"

using namespace OpenEra::Loader;
using OpenEra::Core::Logger;
using OpenEra::Core::LogRecord;
using OpenEra::Core::LogSink;

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

// Optional .edata section (ImageBuilder::WithExports): RVA 0x3000, file 0x600.
constexpr uint32_t kEdataRva = 0x3000;
constexpr uint32_t kEdataFileOffset = 0x600;
constexpr uint32_t kEdataRawSize = 0x200;
constexpr size_t kFileSizeWithExports = 0x800;
constexpr uint32_t kExportDirectorySize = 0x180;
constexpr uint32_t kExportAddressesRva = 0x3040;
constexpr uint32_t kExportNamePointersRva = 0x3060;
constexpr uint32_t kExportOrdinalsRva = 0x3070;
constexpr uint32_t kExportDllNameRva = 0x3100;
constexpr uint32_t kForwarderRva = 0x3120;
constexpr uint32_t kAliasNameRva = 0x3140;
constexpr uint32_t kMainNameRva = 0x3150;
constexpr uint32_t kForwardedNameRva = 0x3160;
constexpr uint32_t kZeroBytesRva = 0x3170;

constexpr size_t IdataFileOffset(uint32_t rva) noexcept
{
    return size_t{rva} - kIdataRva + kIdataFileOffset;
}

constexpr size_t EdataFileOffset(uint32_t rva) noexcept
{
    return size_t{rva} - kEdataRva + kEdataFileOffset;
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

    // Exports: ordinal base 5; slot 1 unused; slot 0 named twice; slot 3 forwarded.
    bool WithExports = false;
    DataDirectory ExportDirectory{.VirtualAddress = kEdataRva, .Size = kExportDirectorySize};
    ExportDirectoryTable ExportTable{.ExportFlags = 0,
                                     .TimeDateStamp = 0,
                                     .MajorVersion = 0,
                                     .MinorVersion = 0,
                                     .NameRva = kExportDllNameRva,
                                     .OrdinalBase = 5,
                                     .AddressTableEntries = 4,
                                     .NamePointerCount = 3,
                                     .ExportAddressTableRva = kExportAddressesRva,
                                     .NamePointerRva = kExportNamePointersRva,
                                     .OrdinalTableRva = kExportOrdinalsRva};
    std::array<uint32_t, 4> ExportAddresses{kTextRva, 0, kTextRva + 8, kForwarderRva};
    std::array<uint32_t, 3> ExportNamePointers{kAliasNameRva, kForwardedNameRva, kMainNameRva};
    std::array<uint16_t, 3> ExportOrdinals{0, 3, 0};

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
        auto fileHeader = FileHeader;
        auto directories = Directories;
        auto sections = Sections;
        if (WithExports)
        {
            ++fileHeader.SectionCount;
            directories.at(static_cast<size_t>(DataDirectoryIndex::Export)) = ExportDirectory;
            sections.push_back({.Name = MakeSectionName(".edata"),
                                .VirtualSize = kEdataRawSize,
                                .VirtualAddress = kEdataRva,
                                .SizeOfRawData = kEdataRawSize,
                                .PointerToRawData = kEdataFileOffset,
                                .PointerToRelocations = 0,
                                .PointerToLineNumbers = 0,
                                .RelocationCount = 0,
                                .LineNumberCount = 0,
                                .Characteristics = 0x4000'0040}); // Initialized data, read.
        }

        std::vector<std::byte> bytes(WithExports ? kFileSizeWithExports : kFileSize);
        Put(bytes, 0, Dos);
        Put(bytes, kPeHeaderOffset, Signature);
        Put(bytes, kFileHeaderOffset, fileHeader);
        Put(bytes, kOptionalHeaderOffset, Optional);
        Put(bytes, kDataDirectoryOffset, directories);

        size_t sectionOffset = kSectionTableOffset;
        for (const auto& section : sections)
        {
            Put(bytes, sectionOffset, section);
            sectionOffset += sizeof(SectionHeader);
        }

        if (WithExports)
        {
            Put(bytes, EdataFileOffset(kEdataRva), ExportTable);
            Put(bytes, EdataFileOffset(kExportAddressesRva), ExportAddresses);
            Put(bytes, EdataFileOffset(kExportNamePointersRva), ExportNamePointers);
            Put(bytes, EdataFileOffset(kExportOrdinalsRva), ExportOrdinals);
            PutString(bytes, EdataFileOffset(kExportDllNameRva), "Game.dll");
            PutString(bytes, EdataFileOffset(kForwarderRva), "KERNEL32.CreateFileW");
            PutString(bytes, EdataFileOffset(kAliasNameRva), "AliasOfMain");
            PutString(bytes, EdataFileOffset(kMainNameRva), "MainEntry");
            PutString(bytes, EdataFileOffset(kForwardedNameRva), "Forwarded");
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

class CaptureSink final : public LogSink
{
public:
    void Write(const LogRecord& record) override { m_messages.emplace_back(record.Message); }

    [[nodiscard]] bool Contains(std::string_view text) const
    {
        return std::ranges::any_of(m_messages, [text](const std::string& message)
                                   { return message.find(text) != std::string::npos; });
    }

    [[nodiscard]] bool Empty() const noexcept { return m_messages.empty(); }

private:
    std::vector<std::string> m_messages;
};

struct LoggedParse
{
    Logger Log;
    std::shared_ptr<CaptureSink> Sink = std::make_shared<CaptureSink>();

    LoggedParse() { Log.AddSink(Sink); }

    PeParseResult operator()(const std::vector<std::byte>& bytes) { return ParsePe(std::span(bytes), Log); }
};
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
    CHECK(ToString(PeError::BadExportTable) == "BadExportTable");
    CHECK(ToString(PeError::OutOfMemory) == "OutOfMemory");
}

TEST_CASE("ParsePe reads named, ordinal-only, aliased and forwarded exports", "[PeReader]")
{
    ImageBuilder builder;
    builder.WithExports = true;
    LoggedParse parse;
    const auto result = parse(builder.Build());
    REQUIRE(result.Error == PeError::None);
    CHECK(parse.Sink->Empty());

    const auto& image = result.Image.value();
    CHECK(image.ExportName == "Game.dll");
    REQUIRE(image.Exports.size() == 4);

    const auto& main = image.Exports.at(0);
    CHECK(main.Ordinal == 5);
    CHECK(main.Rva == kTextRva);
    CHECK(main.Name == "AliasOfMain");
    CHECK(main.Forwarder.empty());

    const auto& ordinalOnly = image.Exports.at(1); // Slot 1 is unused, so ordinal 6 is skipped.
    CHECK(ordinalOnly.Ordinal == 7);
    CHECK(ordinalOnly.Rva == kTextRva + 8);
    CHECK(ordinalOnly.Name.empty());

    const auto& forwarded = image.Exports.at(2);
    CHECK(forwarded.Ordinal == 8);
    CHECK(forwarded.Name == "Forwarded");
    CHECK(forwarded.Forwarder == "KERNEL32.CreateFileW");

    const auto& alias = image.Exports.at(3);
    CHECK(alias.Ordinal == 5);
    CHECK(alias.Rva == kTextRva);
    CHECK(alias.Name == "MainEntry");

    CHECK(image.Imports.size() == 2);
    CHECK(image.Sections.size() == 3);
}

TEST_CASE("ParsePe export boundary conditions", "[PeReader]")
{
    ImageBuilder builder;
    builder.WithExports = true;

    SECTION("no export directory")
    {
        builder.WithExports = false;
        const auto result = Parse(builder.Build());
        REQUIRE(result.Error == PeError::None);
        CHECK(result.Image->Exports.empty());
        CHECK(result.Image->ExportName.empty());
    }

    SECTION("ordinal-only exports")
    {
        builder.ExportTable.NamePointerCount = 0;
        const auto result = Parse(builder.Build());
        REQUIRE(result.Error == PeError::None);
        REQUIRE(result.Image->Exports.size() == 3);
        CHECK(
            std::ranges::all_of(result.Image->Exports, [](const PeExportEntry& entry) { return entry.Name.empty(); }));
    }

    SECTION("empty address table")
    {
        builder.ExportTable.AddressTableEntries = 0;
        builder.ExportTable.NamePointerCount = 0;
        const auto result = Parse(builder.Build());
        REQUIRE(result.Error == PeError::None);
        CHECK(result.Image->Exports.empty());
        CHECK(result.Image->ExportName == "Game.dll");
    }

    SECTION("no DLL name")
    {
        builder.ExportTable.NameRva = 0;
        const auto result = Parse(builder.Build());
        REQUIRE(result.Error == PeError::None);
        CHECK(result.Image->ExportName.empty());
    }

    SECTION("largest ordinal base that does not overflow")
    {
        builder.ExportTable.OrdinalBase = 0xFFFF'FFFF - 3;
        const auto result = Parse(builder.Build());
        REQUIRE(result.Error == PeError::None);
        CHECK(result.Image->Exports.at(2).Ordinal == 0xFFFF'FFFF);
    }

    SECTION("directory size that excludes the forwarder string")
    {
        builder.ExportDirectory.Size = kForwarderRva - kEdataRva;
        const auto result = Parse(builder.Build());
        REQUIRE(result.Error == PeError::None);
        CHECK(result.Image->Exports.at(2).Forwarder.empty());
        CHECK(result.Image->Exports.at(2).Rva == kForwarderRva);
    }
}

TEST_CASE("ParsePe rejects malformed export tables and logs why", "[PeReader]")
{
    ImageBuilder builder;
    builder.WithExports = true;
    LoggedParse parse;

    SECTION("export directory outside every section")
    {
        builder.ExportDirectory.VirtualAddress = 0x9000;
        CHECK(parse(builder.Build()).Error == PeError::BadExportDirectory);
        CHECK(parse.Sink->Contains("export directory at RVA 0x9000"));
    }

    SECTION("ordinal base overflows")
    {
        builder.ExportTable.OrdinalBase = 0xFFFF'FFFF - 2;
        CHECK(parse(builder.Build()).Error == PeError::BadExportDirectory);
        CHECK(parse.Sink->Contains("overflows"));
    }

    SECTION("too many exports")
    {
        builder.ExportTable.AddressTableEntries = kMaxExports + 1;
        CHECK(parse(builder.Build()).Error == PeError::TooManyExports);
    }

    SECTION("DLL name unmapped")
    {
        builder.ExportTable.NameRva = 0x9000;
        CHECK(parse(builder.Build()).Error == PeError::BadExportName);
        CHECK(parse.Sink->Contains("export DLL name"));
    }

    SECTION("address table unmapped")
    {
        builder.ExportTable.ExportAddressTableRva = 0x9000;
        CHECK(parse(builder.Build()).Error == PeError::BadExportTable);
        CHECK(parse.Sink->Contains("export address 0 at RVA 0x9000"));
    }

    SECTION("address table runs past the section's file data")
    {
        builder.ExportTable.ExportAddressTableRva = kEdataRva + kEdataRawSize - 8;
        CHECK(parse(builder.Build()).Error == PeError::BadExportTable);
    }

    SECTION("ordinal table entry past the address table")
    {
        builder.ExportOrdinals.at(1) = 4;
        CHECK(parse(builder.Build()).Error == PeError::BadExportTable);
        CHECK(parse.Sink->Contains("unused address slot 4"));
    }

    SECTION("name refers to an unused slot")
    {
        builder.ExportOrdinals.at(1) = 1;
        CHECK(parse(builder.Build()).Error == PeError::BadExportTable);
    }

    SECTION("name pointer table unmapped")
    {
        builder.ExportTable.NamePointerRva = 0x9000;
        CHECK(parse(builder.Build()).Error == PeError::BadExportTable);
    }

    SECTION("export name unmapped")
    {
        builder.ExportNamePointers.at(2) = 0x9000;
        CHECK(parse(builder.Build()).Error == PeError::BadExportName);
        CHECK(parse.Sink->Contains("export name 2 has a bad RVA 0x9000"));
    }

    SECTION("empty export name")
    {
        builder.ExportNamePointers.at(2) = kZeroBytesRva;
        CHECK(parse(builder.Build()).Error == PeError::BadExportName);
    }

    SECTION("empty forwarder string")
    {
        builder.ExportAddresses.at(3) = kZeroBytesRva;
        CHECK(parse(builder.Build()).Error == PeError::BadExportName);
        CHECK(parse.Sink->Contains("forwarder"));
    }
}

TEST_CASE("ParsePe logs malformed import and header data", "[PeReader]")
{
    ImageBuilder builder;
    LoggedParse parse;

    SECTION("unmapped import descriptor")
    {
        builder.Directories.at(static_cast<size_t>(DataDirectoryIndex::Import)).VirtualAddress = 0x9000;
        CHECK(parse(builder.Build()).Error == PeError::BadImportDirectory);
        CHECK(parse.Sink->Contains("import descriptor 0 at RVA 0x9000"));
    }

    SECTION("bad DLL name")
    {
        builder.Descriptors.at(1).NameRva = 0x9000;
        CHECK(parse(builder.Build()).Error == PeError::BadImportName);
        CHECK(parse.Sink->Contains("import descriptor 1 has a bad DLL name RVA 0x9000"));
    }

    SECTION("bad hint/name entry")
    {
        builder.KernelLookup.at(0) = 0x9000;
        CHECK(parse(builder.Build()).Error == PeError::BadImportName);
        CHECK(parse.Sink->Contains("KERNEL32.dll import 0 has a bad hint/name RVA 0x9000"));
    }

    SECTION("reserved thunk bits")
    {
        builder.KernelLookup.at(0) |= 0x0000'0001'0000'0000ULL;
        CHECK(parse(builder.Build()).Error == PeError::BadImportThunk);
        CHECK(parse.Sink->Contains("reserved bits"));
    }

    SECTION("bad header")
    {
        builder.Dos.Signature = 0;
        CHECK(parse(builder.Build()).Error == PeError::BadDosSignature);
        CHECK(parse.Sink->Contains("invalid headers (BadDosSignature)"));
    }
}

TEST_CASE("RvaToFileOffset section edge cases", "[PeReader]")
{
    ImageBuilder builder;

    SECTION("VirtualSize of zero falls back to SizeOfRawData")
    {
        builder.Sections.at(0).VirtualSize = 0;
        const auto result = Parse(builder.Build());
        REQUIRE(result.Image.has_value());
        CHECK(RvaToFileOffset(*result.Image, kTextRva + 0x1FF) == kTextFileOffset + 0x1FFu);
        CHECK_FALSE(RvaToFileOffset(*result.Image, kTextRva + 0x200).has_value());
    }

    SECTION("first and last mapped byte of each section")
    {
        builder.WithExports = true;
        const auto result = Parse(builder.Build());
        REQUIRE(result.Image.has_value());
        const auto& image = *result.Image;
        CHECK(RvaToFileOffset(image, kTextRva) == kTextFileOffset);
        CHECK(RvaToFileOffset(image, kIdataRva) == kIdataFileOffset);
        CHECK(RvaToFileOffset(image, kEdataRva) == kEdataFileOffset);
        CHECK(RvaToFileOffset(image, kEdataRva + kEdataRawSize - 1) == kEdataFileOffset + kEdataRawSize - 1);
        CHECK_FALSE(RvaToFileOffset(image, kEdataRva + kEdataRawSize).has_value());
    }
}

TEST_CASE("ParsePe with exports never reads past a truncated buffer", "[PeReader]")
{
    ImageBuilder builder;
    builder.WithExports = true;
    const auto full = builder.Build();
    const auto fullSpan = std::span(full);
    Logger quiet;

    for (size_t length = 0; length <= full.size(); ++length)
    {
        INFO("length = " << length);
        const auto result = ParsePe(fullSpan.first(length), quiet);
        REQUIRE(result.Image.has_value() == (result.Error == PeError::None));
        if (length <= EdataFileOffset(kExportNamePointersRva))
        {
            REQUIRE(result.Error != PeError::None);
        }
    }
    CHECK(ParsePe(fullSpan, quiet).Error == PeError::None);
}
