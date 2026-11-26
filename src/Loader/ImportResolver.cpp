#include "ImportResolver.h"

#include <algorithm>
#include <array>
#include <bit>
#include <deque>
#include <new>
#include <span>
#include <utility>

#include "Core/Logger.h"
#include "Kernel/ShimGuard.h"
#include "Loader/HostModules.h"

namespace OpenEra::Loader
{
using Kernel::StubKind;

namespace
{
struct StubEntry
{
    StubKind Kind = StubKind::Win32Import;
    std::string Module;
    std::string Symbol;
    std::optional<uint16_t> Ordinal;
    uint64_t ReturnValue = 0;
    Kernel::StubRegistry* Registry = nullptr;
};
} // namespace

class ImportStubs
{
public:
    std::deque<StubEntry> Entries; // Stable addresses: thunks embed them.
    std::unique_ptr<ImageMemory> Code;
};

namespace
{
using Core::Logger;

constexpr size_t kThunkSize = 32;
constexpr std::string_view kStubNote = "unresolved import";

// Target of every generated thunk; receives its StubEntry in RCX (first argument).
uint64_t StubDispatch(const StubEntry& entry) noexcept
{
    if (entry.Ordinal)
    {
        entry.Registry->RecordHit(entry.Kind, entry.Module, *entry.Ordinal, kStubNote);
    }
    else
    {
        entry.Registry->RecordHit(entry.Kind, entry.Module, entry.Symbol, kStubNote);
    }
    return entry.ReturnValue;
}

// mov rcx, imm64 (entry); mov rax, imm64 (StubDispatch); jmp rax; int3 padding.
// A tail jump, so the thunk never appears on the stack and needs no unwind data.
constexpr std::array<std::byte, kThunkSize> MakeThunk(uint64_t entryAddress, uint64_t dispatchAddress) noexcept
{
    constexpr std::byte kInt3{0xCC};
    std::array<std::byte, kThunkSize> code{
        std::byte{0x48}, std::byte{0xB9}, kInt3, kInt3, kInt3, kInt3, kInt3, kInt3, kInt3, kInt3, // mov rcx, imm64
        std::byte{0x48}, std::byte{0xB8}, kInt3, kInt3, kInt3, kInt3, kInt3, kInt3, kInt3, kInt3, // mov rax, imm64
        std::byte{0xFF}, std::byte{0xE0},                                                         // jmp rax
        kInt3,           kInt3,           kInt3, kInt3, kInt3, kInt3, kInt3, kInt3, kInt3, kInt3};
    const auto out = std::span(code);
    std::ranges::copy(std::bit_cast<std::array<std::byte, sizeof(uint64_t)>>(entryAddress), out.subspan(2).begin());
    std::ranges::copy(std::bit_cast<std::array<std::byte, sizeof(uint64_t)>>(dispatchAddress), out.subspan(12).begin());
    return code;
}

constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept
{
    return (value + alignment - 1) / alignment * alignment;
}

struct PendingStub
{
    size_t Binding = 0;
    const StubEntry* Entry = nullptr;
};

std::optional<uint64_t> FindIn(SymbolSource* source, std::string_view module, const PeImportEntry& entry)
{
    if (source == nullptr)
    {
        return std::nullopt;
    }
    return entry.Ordinal ? source->FindExport(module, *entry.Ordinal) : source->FindExport(module, entry.Name);
}

void BindModule(const PeImportModule& module, bool delayed, const Kernel::ShimRegistry& shims,
                Kernel::StubRegistry& stubs, const ImportSources& sources, BoundImports& bound,
                std::vector<PendingStub>& pending, std::vector<uint64_t>& slots)
{
    const auto* shimModule = shims.FindModule(module.DllName);
    const StubKind kind = shimModule != nullptr ? shimModule->Kind() : StubKindForModule(module.DllName);

    for (size_t index = 0; index < module.Entries.size(); ++index)
    {
        const auto& entry = module.Entries.at(index);
        ImportBinding binding{.Module = module.DllName,
                              .Symbol = entry.Name,
                              .Ordinal = entry.Ordinal,
                              .Address = 0,
                              .Source = ImportSource::Stub,
                              .Delayed = delayed};

        const auto shim =
            entry.Ordinal ? shims.Resolve(module.DllName, *entry.Ordinal) : shims.Resolve(module.DllName, entry.Name);
        if (const auto title = FindIn(sources.TitleModules, module.DllName, entry))
        {
            binding.Address = *title;
            binding.Source = ImportSource::TitleModule;
        }
        else if (shim)
        {
            binding.Address = std::bit_cast<uint64_t>(shim->Export->Proc);
            binding.Source = ImportSource::Shim;
        }
        else if (const auto host = FindIn(sources.Host, module.DllName, entry))
        {
            binding.Address = *host;
            binding.Source = ImportSource::Host;
        }
        else
        {
            const auto& stub =
                bound.Stubs->Entries.emplace_back(StubEntry{.Kind = kind,
                                                            .Module = module.DllName,
                                                            .Symbol = entry.Name,
                                                            .Ordinal = entry.Ordinal,
                                                            .ReturnValue = StubReturnValue(kind, module.DllName),
                                                            .Registry = &stubs});
            pending.push_back({.Binding = bound.Bindings.size(), .Entry = &stub});
        }

        bound.Bindings.push_back(std::move(binding));
        slots.push_back(module.ImportAddressTableRva + (index * sizeof(uint64_t)));
    }
}

ResolveError GenerateStubs(BoundImports& bound, const std::vector<PendingStub>& pending, ImageAllocator& allocator,
                           Logger& logger)
{
    if (pending.empty())
    {
        return ResolveError::None;
    }

    const size_t size = AlignUp(pending.size() * kThunkSize, kPageSize);
    bound.Stubs->Code = allocator.Allocate(0, size);
    if (!bound.Stubs->Code || bound.Stubs->Code->Bytes().size() < size)
    {
        logger.Warn("Imports: could not allocate {:#x} bytes for {} stubs", size, pending.size());
        return ResolveError::StubAllocationFailed;
    }

    const auto dispatch = std::bit_cast<uint64_t>(&StubDispatch);
    const auto code = bound.Stubs->Code->Bytes();
    const uint64_t base = bound.Stubs->Code->Base();
    for (size_t index = 0; index < pending.size(); ++index)
    {
        const auto& stub = pending.at(index);
        const auto thunk = MakeThunk(std::bit_cast<uint64_t>(stub.Entry), dispatch);
        std::ranges::copy(thunk, code.subspan(index * kThunkSize).begin());
        bound.Bindings.at(stub.Binding).Address = base + (index * kThunkSize);
    }

    if (!bound.Stubs->Code->Protect(0, size, PageProtection::ExecuteRead))
    {
        logger.Warn("Imports: could not make the stub code executable");
        return ResolveError::ProtectionFailed;
    }
    return ResolveError::None;
}

ResolveResult ResolveImportsImpl(const MappedImage& image, const PeImage& pe, const Kernel::ShimRegistry& shims,
                                 Kernel::StubRegistry& stubs, ImageAllocator& stubAllocator, Logger& logger,
                                 const ImportSources& sources)
{
    if (!image.Memory)
    {
        logger.Warn("Imports: image is not mapped");
        return {.Error = ResolveError::BadImportTable, .Imports = std::nullopt};
    }

    BoundImports bound;
    bound.Stubs = std::make_unique<ImportStubs>();
    std::vector<PendingStub> pending;
    std::vector<uint64_t> slots; // IAT slot RVA for each binding.
    for (const auto& module : pe.Imports)
    {
        BindModule(module, false, shims, stubs, sources, bound, pending, slots);
    }
    for (const auto& module : pe.DelayImports)
    {
        BindModule(module, true, shims, stubs, sources, bound, pending, slots);
    }

    if (bound.Bindings.empty())
    {
        return {.Error = ResolveError::None, .Imports = std::move(bound)};
    }

    const auto memory = image.Memory->Bytes();
    for (const uint64_t slot : slots)
    {
        if (slot > memory.size() || memory.size() - slot < sizeof(uint64_t))
        {
            logger.Warn("Imports: address table slot at RVA {:#x} is outside the image", slot);
            return {.Error = ResolveError::BadImportTable, .Imports = std::nullopt};
        }
    }

    if (const auto error = GenerateStubs(bound, pending, stubAllocator, logger); error != ResolveError::None)
    {
        return {.Error = error, .Imports = std::nullopt};
    }

    if (!image.Memory->Protect(0, memory.size(), PageProtection::ReadWrite))
    {
        logger.Warn("Imports: could not make the image writable");
        return {.Error = ResolveError::ProtectionFailed, .Imports = std::nullopt};
    }
    for (size_t index = 0; index < slots.size(); ++index)
    {
        const auto address = std::bit_cast<std::array<std::byte, sizeof(uint64_t)>>(bound.Bindings.at(index).Address);
        std::ranges::copy(address, memory.subspan(slots.at(index)).begin());
    }
    if (ProtectImage(pe, *image.Memory, logger) != MapError::None)
    {
        return {.Error = ResolveError::ProtectionFailed, .Imports = std::nullopt};
    }

    logger.Info("Imports: {} from title modules, {} shimmed, {} passed through, {} stubbed",
                bound.CountFrom(ImportSource::TitleModule), bound.CountFrom(ImportSource::Shim),
                bound.CountFrom(ImportSource::Host), bound.CountFrom(ImportSource::Stub));
    return {.Error = ResolveError::None, .Imports = std::move(bound)};
}
} // namespace

BoundImports::BoundImports() noexcept = default;

BoundImports::~BoundImports() = default;
BoundImports::BoundImports(BoundImports&&) noexcept = default;
BoundImports& BoundImports::operator=(BoundImports&&) noexcept = default;

size_t BoundImports::StubbedCount() const noexcept
{
    return Stubs ? Stubs->Entries.size() : 0;
}

size_t BoundImports::CountFrom(ImportSource source) const noexcept
{
    size_t count = 0;
    for (const auto& binding : Bindings)
    {
        count += binding.Source == source ? 1 : 0;
    }
    return count;
}

std::string_view ToString(ImportSource source) noexcept
{
    switch (source)
    {
    case ImportSource::TitleModule:
        return "TitleModule";
    case ImportSource::Shim:
        return "Shim";
    case ImportSource::Host:
        return "Host";
    case ImportSource::Stub:
        return "Stub";
    }
    return "Unknown";
}

StubKind StubKindForModule(std::string_view module)
{
    static const std::array<std::string_view, 2> kGameCoreModules = {"xgameruntime", "gameinput"};
    return std::ranges::find(kGameCoreModules, ModuleKey(module)) != kGameCoreModules.end() ? StubKind::GameCoreApi
                                                                                            : StubKind::Win32Import;
}

uint64_t StubReturnValue(StubKind kind, std::string_view module)
{
    // COM and DirectX entry points report failure through an HRESULT, so 0 would read as success.
    static const std::array<std::string_view, 7> kHResultModules = {"d3d12",     "d3d12_x",   "dxgi",    "dstorage",
                                                                    "xaudio2_8", "xaudio2_9", "xapobase"};
    if (kind == StubKind::Win32Import && std::ranges::find(kHResultModules, ModuleKey(module)) != kHResultModules.end())
    {
        return DefaultStubReturnValue(StubKind::GameCoreApi);
    }
    return DefaultStubReturnValue(kind);
}

uint64_t DefaultStubReturnValue(StubKind kind) noexcept
{
    switch (kind)
    {
    case StubKind::EraApi:
    case StubKind::GameCoreApi:
        return std::bit_cast<uint32_t>(Kernel::kHResultNotImplemented);
    case StubKind::NtSyscall:
        return std::bit_cast<uint32_t>(Kernel::kStatusNotImplemented);
    case StubKind::Win32Import:
        return 0;
    }
    return 0;
}

std::string_view ToString(ResolveError error) noexcept
{
    switch (error)
    {
    case ResolveError::None:
        return "None";
    case ResolveError::BadImportTable:
        return "BadImportTable";
    case ResolveError::StubAllocationFailed:
        return "StubAllocationFailed";
    case ResolveError::ProtectionFailed:
        return "ProtectionFailed";
    case ResolveError::OutOfMemory:
        return "OutOfMemory";
    }
    return "Unknown";
}

ResolveResult ResolveImports(const MappedImage& image, const PeImage& pe, const Kernel::ShimRegistry& shims,
                             Kernel::StubRegistry& stubs, ImageAllocator& stubAllocator, Core::Logger& logger,
                             ImportSources sources) noexcept
{
    try
    {
        return ResolveImportsImpl(image, pe, shims, stubs, stubAllocator, logger, sources);
    }
    catch (const std::bad_alloc&)
    {
        return {.Error = ResolveError::OutOfMemory, .Imports = std::nullopt};
    }
}

ResolveResult ResolveImports(const MappedImage& image, const PeImage& pe, const Kernel::ShimRegistry& shims,
                             Kernel::StubRegistry& stubs, ImageAllocator& stubAllocator, Core::Logger& logger) noexcept
{
    return ResolveImports(image, pe, shims, stubs, stubAllocator, logger, ImportSources{});
}
} // namespace OpenEra::Loader
