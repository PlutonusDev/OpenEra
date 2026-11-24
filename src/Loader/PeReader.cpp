#include "PeReader.h"

#include <new>
#include <utility>

#include "Core/ByteReader.h"

namespace OpenEra::Loader
{
namespace
{
using Core::ByteReader;

// Import lookup table entry (by name): bits 31-62 are reserved and must be zero.
constexpr uint64_t kImportReservedBits = 0x7FFF'FFFF'8000'0000ULL;
constexpr uint64_t kImportOrdinalMask = 0xFFFFULL;

// RVAs are 32-bit in the format, but table walks compute rva + index * size,
// so internal lookups take 64-bit values and cannot wrap.
std::optional<size_t> MapRva(const PeImage& image, uint64_t rva) noexcept
{
    if (rva < image.OptionalHeader.SizeOfHeaders)
    {
        return rva;
    }

    for (const auto& section : image.Sections)
    {
        const uint64_t start = section.VirtualAddress;
        const uint64_t virtualSize = section.VirtualSize != 0 ? section.VirtualSize : section.SizeOfRawData;
        if (rva < start || rva - start >= virtualSize)
        {
            continue;
        }

        const uint64_t offsetInSection = rva - start;
        if (offsetInSection >= section.SizeOfRawData)
        {
            return std::nullopt; // Zero-filled tail, not present in the file.
        }
        return section.PointerToRawData + offsetInSection;
    }
    return std::nullopt;
}

template <typename T>
std::optional<T> ReadAtRva(std::span<const std::byte> file, const PeImage& image, uint64_t rva) noexcept
{
    const auto offset = MapRva(image, rva);
    if (!offset)
    {
        return std::nullopt;
    }

    ByteReader reader(file);
    if (!reader.Seek(*offset))
    {
        return std::nullopt;
    }
    return reader.ReadStruct<T>();
}

// Reads a NUL-terminated ASCII string. Fails if it is unterminated within
// kMaxImportNameLength bytes or runs off the end of the file.
std::optional<std::string> ReadNameAtRva(std::span<const std::byte> file, const PeImage& image, uint64_t rva)
{
    const auto offset = MapRva(image, rva);
    if (!offset)
    {
        return std::nullopt;
    }

    ByteReader reader(file);
    if (!reader.Seek(*offset))
    {
        return std::nullopt;
    }

    std::string name;
    while (name.size() < kMaxImportNameLength)
    {
        const auto character = reader.ReadStruct<char>();
        if (!character)
        {
            return std::nullopt;
        }
        if (*character == '\0')
        {
            return name;
        }
        name.push_back(*character);
    }
    return std::nullopt;
}

PeError ParseHeaders(std::span<const std::byte> file, PeImage& image)
{
    ByteReader reader(file);

    const auto dosHeader = reader.ReadStruct<DosHeader>();
    if (!dosHeader)
    {
        return PeError::TruncatedDosHeader;
    }
    if (dosHeader->Signature != kDosSignature)
    {
        return PeError::BadDosSignature;
    }
    if (!reader.Seek(dosHeader->PeHeaderOffset))
    {
        return PeError::BadPeHeaderOffset;
    }

    const auto signature = reader.ReadU32();
    if (!signature || *signature != kPeSignature)
    {
        return PeError::BadPeSignature;
    }

    const auto fileHeader = reader.ReadStruct<CoffFileHeader>();
    if (!fileHeader)
    {
        return PeError::TruncatedFileHeader;
    }
    image.FileHeader = *fileHeader;

    // Check the magic first, so a PE32 image reports as unsupported rather
    // than as a malformed PE32+ header.
    const size_t optionalHeaderOffset = reader.Offset();
    const auto magic = reader.ReadU16();
    if (fileHeader->OptionalHeaderSize < sizeof(OptionalHeader64) || !magic || *magic != kOptionalHeaderMagicPe32Plus)
    {
        return PeError::UnsupportedOptionalHeader;
    }
    if (!reader.Seek(optionalHeaderOffset))
    {
        return PeError::TruncatedOptionalHeader;
    }

    const auto optionalHeader = reader.ReadStruct<OptionalHeader64>();
    if (!optionalHeader)
    {
        return PeError::TruncatedOptionalHeader;
    }
    image.OptionalHeader = *optionalHeader;

    // At most 16 directories, and they must fit in the declared optional header size.
    const uint32_t directoryCount = optionalHeader->NumberOfRvaAndSizes;
    if (directoryCount > kMaxDataDirectories ||
        sizeof(OptionalHeader64) + (directoryCount * sizeof(DataDirectory)) > fileHeader->OptionalHeaderSize)
    {
        return PeError::BadDataDirectoryCount;
    }

    image.DataDirectories.reserve(directoryCount);
    for (uint32_t index = 0; index < directoryCount; ++index)
    {
        const auto directory = reader.ReadStruct<DataDirectory>();
        if (!directory)
        {
            return PeError::TruncatedOptionalHeader;
        }
        image.DataDirectories.push_back(*directory);
    }

    // The section table follows the optional header as sized by the file header,
    // which may be larger than the fields we read.
    if (!reader.Seek(optionalHeaderOffset + fileHeader->OptionalHeaderSize))
    {
        return PeError::TruncatedSectionTable;
    }

    image.Sections.reserve(fileHeader->SectionCount);
    for (uint32_t index = 0; index < fileHeader->SectionCount; ++index)
    {
        const auto section = reader.ReadStruct<SectionHeader>();
        if (!section)
        {
            return PeError::TruncatedSectionTable;
        }
        image.Sections.push_back(*section);
    }

    return PeError::None;
}

PeError ParseImportThunks(std::span<const std::byte> file, const PeImage& image, uint32_t tableRva,
                          std::vector<PeImportEntry>& entries)
{
    for (size_t index = 0;; ++index)
    {
        if (index >= kMaxImportsPerModule)
        {
            return PeError::TooManyImports;
        }

        const uint64_t thunkRva = tableRva + (index * sizeof(uint64_t));
        const auto thunk = ReadAtRva<uint64_t>(file, image, thunkRva);
        if (!thunk)
        {
            return PeError::BadImportThunk;
        }
        if (*thunk == 0)
        {
            return PeError::None;
        }

        PeImportEntry entry;
        if ((*thunk & kImportByOrdinalFlag) != 0)
        {
            entry.Ordinal = static_cast<uint16_t>(*thunk & kImportOrdinalMask);
        }
        else
        {
            if ((*thunk & kImportReservedBits) != 0)
            {
                return PeError::BadImportThunk;
            }

            // Hint/name table entry: a 16-bit hint followed by the NUL-terminated name.
            const auto hint = ReadAtRva<uint16_t>(file, image, *thunk);
            auto name = ReadNameAtRva(file, image, *thunk + sizeof(uint16_t));
            if (!hint || !name || name->empty())
            {
                return PeError::BadImportName;
            }
            entry.Hint = *hint;
            entry.Name = std::move(*name);
        }
        entries.push_back(std::move(entry));
    }
}

PeError ParseImports(std::span<const std::byte> file, PeImage& image)
{
    const auto directory = image.Directory(DataDirectoryIndex::Import);
    if (!directory || directory->VirtualAddress == 0 || directory->Size == 0)
    {
        return PeError::None;
    }

    for (size_t index = 0;; ++index)
    {
        if (index >= kMaxImportModules)
        {
            return PeError::TooManyImports;
        }

        const uint64_t descriptorRva = directory->VirtualAddress + (index * sizeof(ImportDescriptor));
        const auto descriptor = ReadAtRva<ImportDescriptor>(file, image, descriptorRva);
        if (!descriptor)
        {
            return PeError::BadImportDirectory;
        }
        if (descriptor->NameRva == 0 && descriptor->ImportAddressTableRva == 0)
        {
            return PeError::None; // Null terminator entry.
        }

        PeImportModule module;
        module.ImportAddressTableRva = descriptor->ImportAddressTableRva;

        auto dllName = ReadNameAtRva(file, image, descriptor->NameRva);
        if (!dllName || dllName->empty())
        {
            return PeError::BadImportName;
        }
        module.DllName = std::move(*dllName);

        // Prefer the lookup table; some linkers leave it zero and only fill the
        // address table, which holds the same entries in an unbound file.
        const uint32_t tableRva = descriptor->ImportLookupTableRva != 0 ? descriptor->ImportLookupTableRva
                                                                        : descriptor->ImportAddressTableRva;
        if (const auto error = ParseImportThunks(file, image, tableRva, module.Entries); error != PeError::None)
        {
            return error;
        }

        image.Imports.push_back(std::move(module));
    }
}
} // namespace

std::string_view ToString(PeError error) noexcept
{
    switch (error)
    {
    case PeError::None:
        return "None";
    case PeError::TruncatedDosHeader:
        return "TruncatedDosHeader";
    case PeError::BadDosSignature:
        return "BadDosSignature";
    case PeError::BadPeHeaderOffset:
        return "BadPeHeaderOffset";
    case PeError::BadPeSignature:
        return "BadPeSignature";
    case PeError::TruncatedFileHeader:
        return "TruncatedFileHeader";
    case PeError::UnsupportedOptionalHeader:
        return "UnsupportedOptionalHeader";
    case PeError::TruncatedOptionalHeader:
        return "TruncatedOptionalHeader";
    case PeError::BadDataDirectoryCount:
        return "BadDataDirectoryCount";
    case PeError::TruncatedSectionTable:
        return "TruncatedSectionTable";
    case PeError::BadImportDirectory:
        return "BadImportDirectory";
    case PeError::BadImportName:
        return "BadImportName";
    case PeError::BadImportThunk:
        return "BadImportThunk";
    case PeError::TooManyImports:
        return "TooManyImports";
    case PeError::OutOfMemory:
        return "OutOfMemory";
    }
    return "Unknown";
}

std::optional<DataDirectory> PeImage::Directory(DataDirectoryIndex index) const noexcept
{
    const auto position = static_cast<uint32_t>(index);
    if (position >= DataDirectories.size())
    {
        return std::nullopt;
    }
    return std::span(DataDirectories).subspan(position, 1).front();
}

PeParseResult ParsePe(std::span<const std::byte> file) noexcept
{
    try
    {
        PeImage image;
        if (const auto error = ParseHeaders(file, image); error != PeError::None)
        {
            return {.Error = error, .Image = std::nullopt};
        }
        if (const auto error = ParseImports(file, image); error != PeError::None)
        {
            return {.Error = error, .Image = std::nullopt};
        }
        return {.Error = PeError::None, .Image = std::move(image)};
    }
    catch (const std::bad_alloc&)
    {
        return {.Error = PeError::OutOfMemory, .Image = std::nullopt};
    }
}

std::optional<size_t> RvaToFileOffset(const PeImage& image, uint32_t rva) noexcept
{
    return MapRva(image, rva);
}

std::string_view SectionName(const SectionHeader& section) noexcept
{
    const std::string_view stored(section.Name.data(), section.Name.size());
    return stored.substr(0, stored.find('\0'));
}
} // namespace OpenEra::Loader
