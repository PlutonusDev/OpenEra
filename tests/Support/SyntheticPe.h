#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Loader/HostModules.h"
#include "Loader/ImageMapper.h"
#include "Loader/PeReader.h"

namespace OpenEra::Tests
{
// Builds small but valid PE32+ images with exports, imports and delay imports,
// all in one read-write section at RVA 0x1000. Enough for loader tests.
class SyntheticPe
{
public:
    static constexpr uint32_t kSectionRva = 0x1000;
    static constexpr uint32_t kSectionSize = 0x3000;
    static constexpr uint32_t kHeadersSize = 0x400;

    explicit SyntheticPe(uint64_t imageBase, std::string exportName = {}, bool isDll = true)
        : m_imageBase(imageBase)
        , m_exportName(std::move(exportName))
        , m_isDll(isDll)
        , m_section(kSectionSize)
    {
    }

    // Reserves `size` bytes and stores `value` in the first eight; returns the RVA.
    uint32_t AddData(uint32_t size, uint64_t value)
    {
        const uint32_t rva = Allocate(size);
        Put(rva, value);
        return rva;
    }

    void Export(std::string name, uint32_t rva, uint16_t ordinal)
    {
        m_exports.push_back({.Name = std::move(name), .Ordinal = ordinal, .Rva = rva, .Forwarder = {}});
    }
    void ExportOrdinalOnly(uint32_t rva, uint16_t ordinal)
    {
        m_exports.push_back({.Name = {}, .Ordinal = ordinal, .Rva = rva, .Forwarder = {}});
    }
    void Forward(std::string name, std::string target, uint16_t ordinal)
    {
        m_exports.push_back({.Name = std::move(name), .Ordinal = ordinal, .Rva = 0, .Forwarder = std::move(target)});
    }

    void Import(const std::string& dll, std::string symbol)
    {
        Add(m_imports, dll, {.Name = std::move(symbol), .Ordinal = std::nullopt});
    }
    void Import(const std::string& dll, uint16_t ordinal) { Add(m_imports, dll, {.Name = {}, .Ordinal = ordinal}); }
    void DelayImport(const std::string& dll, uint16_t ordinal)
    {
        Add(m_delayImports, dll, {.Name = {}, .Ordinal = ordinal});
    }

    [[nodiscard]] std::vector<std::byte> Build()
    {
        std::array<Loader::DataDirectory, Loader::kMaxDataDirectories> directories{};
        if (!m_exports.empty())
        {
            directories.at(0) = BuildExports();
        }
        if (!m_imports.empty())
        {
            directories.at(1) = BuildImports();
        }
        if (!m_delayImports.empty())
        {
            directories.at(13) = BuildDelayImports();
        }

        std::vector<std::byte> file(kHeadersSize + kSectionSize);
        Loader::DosHeader dos{};
        dos.Signature = Loader::kDosSignature;
        dos.PeHeaderOffset = 0x40;
        Write(file, 0, dos);
        Write(file, 0x40, Loader::kPeSignature);

        Loader::CoffFileHeader coff{};
        coff.Machine = Loader::kMachineAmd64;
        coff.SectionCount = 1;
        coff.OptionalHeaderSize = sizeof(Loader::OptionalHeader64) + sizeof(directories);
        coff.Characteristics = m_isDll ? 0x2022 : 0x0022;
        Write(file, 0x44, coff);

        Loader::OptionalHeader64 optional{};
        optional.Magic = Loader::kOptionalHeaderMagicPe32Plus;
        optional.ImageBase = m_imageBase;
        optional.SectionAlignment = 0x1000;
        optional.FileAlignment = 0x200;
        optional.SizeOfImage = kSectionRva + kSectionSize;
        optional.SizeOfHeaders = kHeadersSize;
        optional.Subsystem = 2;
        optional.NumberOfRvaAndSizes = Loader::kMaxDataDirectories;
        Write(file, 0x58, optional);
        Write(file, 0x58 + sizeof(optional), directories);

        Loader::SectionHeader section{};
        std::ranges::copy(std::string_view(".data"), section.Name.begin());
        section.VirtualAddress = kSectionRva;
        section.VirtualSize = kSectionSize;
        section.SizeOfRawData = kSectionSize;
        section.PointerToRawData = kHeadersSize;
        section.Characteristics = Loader::kSectionMemoryRead | Loader::kSectionMemoryWrite | 0x40;
        Write(file, 0x58 + sizeof(optional) + sizeof(directories), section);

        std::ranges::copy(m_section, std::span(file).subspan(kHeadersSize).begin());
        return file;
    }

    // IAT slot RVA for an import ("symbol" or "#ordinal"), available after Build.
    [[nodiscard]] uint32_t SlotRva(const std::string& dll, const std::string& symbol) const
    {
        return m_slots.at(Loader::ModuleKey(dll) + "!" + symbol);
    }

private:
    struct ExportItem
    {
        std::string Name;
        uint16_t Ordinal = 0;
        uint32_t Rva = 0;
        std::string Forwarder;
    };

    struct ImportItem
    {
        std::string Name;
        std::optional<uint16_t> Ordinal;
    };

    struct ImportModule
    {
        std::string Dll;
        std::vector<ImportItem> Items;
    };

    static void Add(std::vector<ImportModule>& modules, const std::string& dll, ImportItem item)
    {
        auto it = std::ranges::find_if(modules, [&dll](const ImportModule& module) { return module.Dll == dll; });
        if (it == modules.end())
        {
            modules.push_back({.Dll = dll, .Items = {}});
            it = std::prev(modules.end());
        }
        it->Items.push_back(std::move(item));
    }

    template <typename T>
    static void Write(std::vector<std::byte>& bytes, size_t offset, const T& value)
    {
        std::ranges::copy(std::bit_cast<std::array<std::byte, sizeof(T)>>(value),
                          std::span(bytes).subspan(offset).begin());
    }

    uint32_t Allocate(uint32_t size)
    {
        m_cursor = (m_cursor + 7) & ~uint32_t{7};
        if (m_cursor + size > kSectionSize)
        {
            throw std::length_error("synthetic image section is full");
        }
        const uint32_t rva = kSectionRva + m_cursor;
        m_cursor += size;
        return rva;
    }

    template <typename T>
    void Put(uint32_t rva, const T& value)
    {
        Write(m_section, rva - kSectionRva, value);
    }

    uint32_t PutString(std::string_view text)
    {
        const uint32_t rva = Allocate(static_cast<uint32_t>(text.size() + 1));
        std::ranges::copy(std::as_bytes(std::span(text)), std::span(m_section).subspan(rva - kSectionRva).begin());
        return rva;
    }

    Loader::DataDirectory BuildExports()
    {
        uint16_t maxOrdinal = 0;
        for (const auto& item : m_exports)
        {
            maxOrdinal = std::max(maxOrdinal, item.Ordinal);
        }
        auto named = m_exports;
        std::erase_if(named, [](const ExportItem& item) { return item.Name.empty(); });
        std::ranges::sort(named, {}, &ExportItem::Name);

        const uint32_t directory = Allocate(sizeof(Loader::ExportDirectoryTable));
        const uint32_t addresses = Allocate(4U * maxOrdinal);
        const uint32_t names = Allocate(4U * static_cast<uint32_t>(named.size()));
        const uint32_t ordinals = Allocate(2U * static_cast<uint32_t>(named.size()));
        const uint32_t dllName = PutString(m_exportName.empty() ? "synthetic.dll" : m_exportName);

        for (const auto& item : m_exports)
        {
            const uint32_t target = item.Forwarder.empty() ? item.Rva : PutString(item.Forwarder);
            Put(addresses + (4U * (item.Ordinal - 1U)), target);
        }
        for (size_t index = 0; index < named.size(); ++index)
        {
            const auto& item = named.at(index);
            Put(names + static_cast<uint32_t>(4 * index), PutString(item.Name));
            Put(ordinals + static_cast<uint32_t>(2 * index), static_cast<uint16_t>(item.Ordinal - 1));
        }

        Loader::ExportDirectoryTable table{};
        table.NameRva = dllName;
        table.OrdinalBase = 1;
        table.AddressTableEntries = maxOrdinal;
        table.NamePointerCount = static_cast<uint32_t>(named.size());
        table.ExportAddressTableRva = addresses;
        table.NamePointerRva = names;
        table.OrdinalTableRva = ordinals;
        Put(directory, table);
        return {.VirtualAddress = directory, .Size = kSectionRva + m_cursor - directory};
    }

    uint64_t ThunkFor(const ImportItem& item)
    {
        if (item.Ordinal)
        {
            return Loader::kImportByOrdinalFlag | *item.Ordinal;
        }
        const uint32_t hintName = Allocate(static_cast<uint32_t>(2 + item.Name.size() + 1));
        Put(hintName, uint16_t{0});
        std::ranges::copy(std::as_bytes(std::span(item.Name)),
                          std::span(m_section).subspan(hintName - kSectionRva + 2).begin());
        return hintName;
    }

    void Record(const std::string& dll, const ImportItem& item, uint32_t slot)
    {
        const std::string symbol = item.Ordinal ? "#" + std::to_string(*item.Ordinal) : item.Name;
        m_slots[Loader::ModuleKey(dll) + "!" + symbol] = slot;
    }

    Loader::DataDirectory BuildImports()
    {
        const uint32_t descriptors =
            Allocate(static_cast<uint32_t>(sizeof(Loader::ImportDescriptor) * (m_imports.size() + 1)));
        for (size_t index = 0; index < m_imports.size(); ++index)
        {
            const auto& module = m_imports.at(index);
            const auto count = static_cast<uint32_t>(module.Items.size() + 1);
            const uint32_t lookup = Allocate(8 * count);
            const uint32_t address = Allocate(8 * count);
            for (size_t item = 0; item < module.Items.size(); ++item)
            {
                const uint64_t thunk = ThunkFor(module.Items.at(item));
                Put(lookup + static_cast<uint32_t>(8 * item), thunk);
                Put(address + static_cast<uint32_t>(8 * item), thunk);
                Record(module.Dll, module.Items.at(item), address + static_cast<uint32_t>(8 * item));
            }
            Loader::ImportDescriptor descriptor{};
            descriptor.ImportLookupTableRva = lookup;
            descriptor.NameRva = PutString(module.Dll);
            descriptor.ImportAddressTableRva = address;
            Put(descriptors + static_cast<uint32_t>(sizeof(descriptor) * index), descriptor);
        }
        return {.VirtualAddress = descriptors,
                .Size = static_cast<uint32_t>(sizeof(Loader::ImportDescriptor) * (m_imports.size() + 1))};
    }

    Loader::DataDirectory BuildDelayImports()
    {
        const auto size = static_cast<uint32_t>(sizeof(Loader::DelayLoadDescriptor) * (m_delayImports.size() + 1));
        const uint32_t descriptors = Allocate(size);
        for (size_t index = 0; index < m_delayImports.size(); ++index)
        {
            const auto& module = m_delayImports.at(index);
            const auto count = static_cast<uint32_t>(module.Items.size() + 1);
            const uint32_t names = Allocate(8 * count);
            const uint32_t address = Allocate(8 * count);
            for (size_t item = 0; item < module.Items.size(); ++item)
            {
                Put(names + static_cast<uint32_t>(8 * item), ThunkFor(module.Items.at(item)));
                Record(module.Dll, module.Items.at(item), address + static_cast<uint32_t>(8 * item));
            }
            Loader::DelayLoadDescriptor descriptor{};
            descriptor.Attributes = 1;
            descriptor.NameRva = PutString(module.Dll);
            descriptor.ModuleHandleRva = Allocate(8);
            descriptor.DelayImportAddressTableRva = address;
            descriptor.DelayImportNameTableRva = names;
            Put(descriptors + static_cast<uint32_t>(sizeof(descriptor) * index), descriptor);
        }
        return {.VirtualAddress = descriptors, .Size = size};
    }

    uint64_t m_imageBase;
    std::string m_exportName;
    bool m_isDll;
    std::vector<std::byte> m_section;
    uint32_t m_cursor = 0;
    std::vector<ExportItem> m_exports;
    std::vector<ImportModule> m_imports;
    std::vector<ImportModule> m_delayImports;
    std::map<std::string, uint32_t> m_slots;
};
} // namespace OpenEra::Tests
