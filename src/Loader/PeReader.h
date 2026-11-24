#pragma once

// Reader for PE/COFF images (PE32+ only), written from the public
// "PE Format" specification on Microsoft Learn. Struct and field names are
// our own; layouts and constants follow the documented on-disk format.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OpenEra::Loader
{
// "MZ" at offset 0 of the MS-DOS stub.
constexpr uint16_t kDosSignature = 0x5A4D;
// "PE\0\0" at the offset stored in the DOS header.
constexpr uint32_t kPeSignature = 0x00004550;
constexpr uint16_t kOptionalHeaderMagicPe32 = 0x010B;
constexpr uint16_t kOptionalHeaderMagicPe32Plus = 0x020B;
constexpr uint16_t kMachineAmd64 = 0x8664;
constexpr uint32_t kMaxDataDirectories = 16;

// Data directory indices from the specification.
enum class DataDirectoryIndex : uint8_t
{
    Export = 0,
    Import = 1,
    Resource = 2,
    Exception = 3,
    Certificate = 4,
    BaseRelocation = 5,
    Debug = 6,
    Architecture = 7,
    GlobalPointer = 8,
    Tls = 9,
    LoadConfig = 10,
    BoundImport = 11,
    ImportAddressTable = 12,
    DelayImport = 13,
    ClrRuntime = 14,
    Reserved = 15,
};

#pragma pack(push, 1)

// MS-DOS header. Only the signature and the PE header offset matter to us.
struct DosHeader
{
    uint16_t Signature;
    std::array<uint8_t, 58> Stub;
    uint32_t PeHeaderOffset;
};
static_assert(sizeof(DosHeader) == 64);
static_assert(offsetof(DosHeader, PeHeaderOffset) == 0x3C);

// COFF file header, immediately after the PE signature.
struct CoffFileHeader
{
    uint16_t Machine;
    uint16_t SectionCount;
    uint32_t TimeDateStamp;
    uint32_t SymbolTableOffset;
    uint32_t SymbolCount;
    uint16_t OptionalHeaderSize;
    uint16_t Characteristics;
};
static_assert(sizeof(CoffFileHeader) == 20);

struct DataDirectory
{
    uint32_t VirtualAddress;
    uint32_t Size;
};
static_assert(sizeof(DataDirectory) == 8);

// Fixed part of the PE32+ optional header (standard + Windows-specific fields).
// The data directory array of NumberOfRvaAndSizes entries follows it.
struct OptionalHeader64
{
    uint16_t Magic;
    uint8_t MajorLinkerVersion;
    uint8_t MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
};
static_assert(sizeof(OptionalHeader64) == 112);
static_assert(offsetof(OptionalHeader64, ImageBase) == 24);
static_assert(offsetof(OptionalHeader64, SizeOfStackReserve) == 72);

struct SectionHeader
{
    std::array<char, 8> Name;
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLineNumbers;
    uint16_t RelocationCount;
    uint16_t LineNumberCount;
    uint32_t Characteristics;
};
static_assert(sizeof(SectionHeader) == 40);

// One entry of the import directory table; an all-zero entry ends the table.
struct ImportDescriptor
{
    uint32_t ImportLookupTableRva;
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t NameRva;
    uint32_t ImportAddressTableRva;
};
static_assert(sizeof(ImportDescriptor) == 20);

#pragma pack(pop)

// PE32+ import lookup table entry flag: import by ordinal instead of by name.
constexpr uint64_t kImportByOrdinalFlag = 1ULL << 63;

// Hard limits that stop malformed images from causing unbounded work.
constexpr size_t kMaxImportModules = 4096;
constexpr size_t kMaxImportsPerModule = 65536;
constexpr size_t kMaxImportNameLength = 4096;

enum class PeError : uint8_t
{
    None,
    TruncatedDosHeader,
    BadDosSignature,
    BadPeHeaderOffset,
    BadPeSignature,
    TruncatedFileHeader,
    UnsupportedOptionalHeader, // Missing, or PE32 rather than PE32+.
    TruncatedOptionalHeader,
    BadDataDirectoryCount,
    TruncatedSectionTable,
    BadImportDirectory,
    BadImportName,
    BadImportThunk,
    TooManyImports,
    OutOfMemory,
};

[[nodiscard]] std::string_view ToString(PeError error) noexcept;

struct PeImportEntry
{
    // Set when imported by ordinal; Name and Hint are then unused.
    std::optional<uint16_t> Ordinal;
    uint16_t Hint = 0;
    std::string Name;
};

struct PeImportModule
{
    std::string DllName;
    uint32_t ImportAddressTableRva = 0;
    std::vector<PeImportEntry> Entries;
};

struct PeImage
{
    CoffFileHeader FileHeader{};
    OptionalHeader64 OptionalHeader{};
    std::vector<DataDirectory> DataDirectories;
    std::vector<SectionHeader> Sections;
    std::vector<PeImportModule> Imports;

    // Returns the directory, or nullopt if the image declares fewer directories.
    [[nodiscard]] std::optional<DataDirectory> Directory(DataDirectoryIndex index) const noexcept;
};

struct PeParseResult
{
    PeError Error = PeError::None;
    std::optional<PeImage> Image; // Set exactly when Error == PeError::None.
};

// Parses the headers, section table and import directory of a PE32+ image
// laid out as a file (not mapped). Never reads outside `file`.
[[nodiscard]] PeParseResult ParsePe(std::span<const std::byte> file) noexcept;

// Maps an RVA to an offset in the file. RVAs inside the headers map 1:1.
// Returns nullopt for RVAs outside every section, or in a section's
// zero-filled tail (past SizeOfRawData), which has no file backing.
[[nodiscard]] std::optional<size_t> RvaToFileOffset(const PeImage& image, uint32_t rva) noexcept;

// Section name without its NUL padding. Long names ("/123", a string table
// offset in object files) are returned as stored.
[[nodiscard]] std::string_view SectionName(const SectionHeader& section) noexcept;
} // namespace OpenEra::Loader
