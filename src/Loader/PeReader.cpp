#include "PeReader.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <new>
#include <utility>

#include "Core/ByteReader.h"
#include "Core/Logger.h"

namespace OpenEra::Loader
{
namespace
{
using Core::ByteReader;
using Core::Logger;

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
// kMaxSymbolNameLength bytes or runs off the end of the file.
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
    while (name.size() < kMaxSymbolNameLength)
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

PeError ParseImportThunks(std::span<const std::byte> file, const PeImage& image, Logger& logger,
                          std::string_view dllName, uint32_t tableRva, std::vector<PeImportEntry>& entries)
{
    for (size_t index = 0;; ++index)
    {
        if (index >= kMaxImportsPerModule)
        {
            logger.Warn("PE: {} imports more than {} symbols", dllName, kMaxImportsPerModule);
            return PeError::TooManyImports;
        }

        const uint64_t thunkRva = tableRva + (index * sizeof(uint64_t));
        const auto thunk = ReadAtRva<uint64_t>(file, image, thunkRva);
        if (!thunk)
        {
            logger.Warn("PE: {} import thunk {} at RVA {:#x} is unmapped", dllName, index, thunkRva);
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
                logger.Warn("PE: {} import thunk {} has reserved bits set ({:#x})", dllName, index, *thunk);
                return PeError::BadImportThunk;
            }

            // Hint/name table entry: a 16-bit hint followed by the NUL-terminated name.
            const auto hint = ReadAtRva<uint16_t>(file, image, *thunk);
            auto name = ReadNameAtRva(file, image, *thunk + sizeof(uint16_t));
            if (!hint || !name || name->empty())
            {
                logger.Warn("PE: {} import {} has a bad hint/name RVA {:#x}", dllName, index, *thunk);
                return PeError::BadImportName;
            }
            entry.Hint = *hint;
            entry.Name = std::move(*name);
        }
        entries.push_back(std::move(entry));
    }
}

PeError ParseImports(std::span<const std::byte> file, PeImage& image, Logger& logger)
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
            logger.Warn("PE: more than {} import descriptors", kMaxImportModules);
            return PeError::TooManyImports;
        }

        const uint64_t descriptorRva = directory->VirtualAddress + (index * sizeof(ImportDescriptor));
        const auto descriptor = ReadAtRva<ImportDescriptor>(file, image, descriptorRva);
        if (!descriptor)
        {
            logger.Warn("PE: import descriptor {} at RVA {:#x} is unmapped", index, descriptorRva);
            return PeError::BadImportDirectory;
        }

        // Copied out of the packed struct before logging; the logger takes references.
        const uint32_t nameRva = descriptor->NameRva;
        const uint32_t lookupRva = descriptor->ImportLookupTableRva;
        const uint32_t addressRva = descriptor->ImportAddressTableRva;
        if (nameRva == 0 && addressRva == 0)
        {
            return PeError::None; // Null terminator entry.
        }

        PeImportModule module;
        module.ImportAddressTableRva = addressRva;

        auto dllName = ReadNameAtRva(file, image, nameRva);
        if (!dllName || dllName->empty())
        {
            logger.Warn("PE: import descriptor {} has a bad DLL name RVA {:#x}", index, nameRva);
            return PeError::BadImportName;
        }
        module.DllName = std::move(*dllName);

        // Prefer the lookup table; some linkers leave it zero and only fill the
        // address table, which holds the same entries in an unbound file.
        const uint32_t tableRva = lookupRva != 0 ? lookupRva : addressRva;
        if (const auto error = ParseImportThunks(file, image, logger, module.DllName, tableRva, module.Entries);
            error != PeError::None)
        {
            return error;
        }

        image.Imports.push_back(std::move(module));
    }
}

PeError ParseExports(std::span<const std::byte> file, PeImage& image, Logger& logger)
{
    const auto directory = image.Directory(DataDirectoryIndex::Export);
    if (!directory || directory->VirtualAddress == 0 || directory->Size == 0)
    {
        return PeError::None;
    }

    const uint64_t directoryStart = directory->VirtualAddress;
    const uint64_t directoryEnd = directoryStart + directory->Size;
    const auto table = ReadAtRva<ExportDirectoryTable>(file, image, directoryStart);
    if (!table)
    {
        logger.Warn("PE: export directory at RVA {:#x} is unmapped", directoryStart);
        return PeError::BadExportDirectory;
    }

    const uint32_t nameRva = table->NameRva;
    const uint32_t ordinalBase = table->OrdinalBase;
    const uint32_t addressCount = table->AddressTableEntries;
    const uint32_t nameCount = table->NamePointerCount;
    const uint32_t addressTableRva = table->ExportAddressTableRva;
    const uint32_t namePointerRva = table->NamePointerRva;
    const uint32_t ordinalTableRva = table->OrdinalTableRva;

    if (addressCount > kMaxExports || nameCount > kMaxExports)
    {
        logger.Warn("PE: export table has {} addresses and {} names (limit {})", addressCount, nameCount, kMaxExports);
        return PeError::TooManyExports;
    }
    if (addressCount != 0 && ordinalBase > std::numeric_limits<uint32_t>::max() - (addressCount - 1))
    {
        logger.Warn("PE: export ordinal base {} overflows with {} entries", ordinalBase, addressCount);
        return PeError::BadExportDirectory;
    }

    if (nameRva != 0)
    {
        auto name = ReadNameAtRva(file, image, nameRva);
        if (!name)
        {
            logger.Warn("PE: export DLL name RVA {:#x} is invalid", nameRva);
            return PeError::BadExportName;
        }
        image.ExportName = std::move(*name);
    }

    std::vector<PeExportEntry> slots(addressCount);
    for (uint32_t index = 0; index < addressCount; ++index)
    {
        const uint64_t entryRva = addressTableRva + (index * sizeof(uint32_t));
        const auto address = ReadAtRva<uint32_t>(file, image, entryRva);
        if (!address)
        {
            logger.Warn("PE: export address {} at RVA {:#x} is unmapped", index, entryRva);
            return PeError::BadExportTable;
        }

        auto& slot = slots.at(index);
        slot.Ordinal = ordinalBase + index;
        slot.Rva = *address;
        if (*address >= directoryStart && *address < directoryEnd)
        {
            auto forwarder = ReadNameAtRva(file, image, *address);
            if (!forwarder || forwarder->empty())
            {
                logger.Warn("PE: export {} has a bad forwarder RVA {:#x}", slot.Ordinal, *address);
                return PeError::BadExportName;
            }
            slot.Forwarder = std::move(*forwarder);
        }
    }

    std::vector<PeExportEntry> aliases;
    for (uint32_t index = 0; index < nameCount; ++index)
    {
        const uint64_t pointerRva = namePointerRva + (index * sizeof(uint32_t));
        const uint64_t ordinalRva = ordinalTableRva + (index * sizeof(uint16_t));
        const auto namePointer = ReadAtRva<uint32_t>(file, image, pointerRva);
        const auto slotIndex = ReadAtRva<uint16_t>(file, image, ordinalRva);
        if (!namePointer || !slotIndex)
        {
            logger.Warn("PE: export name {} has an unmapped table entry (name {:#x}, ordinal {:#x})", index, pointerRva,
                        ordinalRva);
            return PeError::BadExportTable;
        }
        if (*slotIndex >= addressCount || slots.at(*slotIndex).Rva == 0)
        {
            logger.Warn("PE: export name {} refers to unused address slot {}", index, *slotIndex);
            return PeError::BadExportTable;
        }

        auto name = ReadNameAtRva(file, image, *namePointer);
        if (!name || name->empty())
        {
            logger.Warn("PE: export name {} has a bad RVA {:#x}", index, *namePointer);
            return PeError::BadExportName;
        }

        auto& slot = slots.at(*slotIndex);
        if (slot.Name.empty())
        {
            slot.Name = std::move(*name);
        }
        else
        {
            auto alias = slot;
            alias.Name = std::move(*name);
            aliases.push_back(std::move(alias));
        }
    }

    image.Exports.reserve(slots.size() + aliases.size());
    for (auto& slot : slots)
    {
        if (slot.Rva != 0)
        {
            image.Exports.push_back(std::move(slot));
        }
    }
    std::ranges::move(aliases, std::back_inserter(image.Exports));
    return PeError::None;
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
    case PeError::BadExportDirectory:
        return "BadExportDirectory";
    case PeError::BadExportTable:
        return "BadExportTable";
    case PeError::BadExportName:
        return "BadExportName";
    case PeError::TooManyExports:
        return "TooManyExports";
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
    return ParsePe(file, Core::DefaultLogger());
}

PeParseResult ParsePe(std::span<const std::byte> file, Core::Logger& logger) noexcept
{
    try
    {
        PeImage image;
        if (const auto error = ParseHeaders(file, image); error != PeError::None)
        {
            logger.Warn("PE: invalid headers ({})", ToString(error));
            return {.Error = error, .Image = std::nullopt};
        }
        if (const auto error = ParseImports(file, image, logger); error != PeError::None)
        {
            return {.Error = error, .Image = std::nullopt};
        }
        if (const auto error = ParseExports(file, image, logger); error != PeError::None)
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
