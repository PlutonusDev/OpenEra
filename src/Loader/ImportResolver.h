#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"
#include "Loader/ImageMapper.h"
#include "Loader/ImageMemory.h"
#include "Loader/PeReader.h"

namespace OpenEra::Core
{
class Logger;
} // namespace OpenEra::Core

namespace OpenEra::Loader
{
// Value an unresolved import returns: E_NOTIMPL for Era / GameCore APIs,
// STATUS_NOT_IMPLEMENTED for syscalls, 0 (FALSE / null) for Win32 imports.
[[nodiscard]] uint64_t DefaultStubReturnValue(Kernel::StubKind kind) noexcept;

// Where an import was bound, in resolution order.
enum class ImportSource : uint8_t
{
    TitleModule, // An export of another image loaded from the title package.
    Shim,        // An OpenEra shim.
    Host,        // The host's own Windows DLL (passthrough).
    Stub,        // A generated stub that records the call.
};

[[nodiscard]] std::string_view ToString(ImportSource source) noexcept;

struct ImportBinding
{
    std::string Module;
    std::string Symbol; // Empty when imported by ordinal.
    std::optional<uint16_t> Ordinal;
    uint64_t Address = 0;
    ImportSource Source = ImportSource::Stub;
    bool Delayed = false;
};

// Something imports can be bound to besides shims: title modules or host DLLs.
// Returns nullopt when the module or export isn't provided by this source.
class SymbolSource
{
public:
    SymbolSource() = default;
    virtual ~SymbolSource() = default;
    SymbolSource(const SymbolSource&) = delete;
    SymbolSource& operator=(const SymbolSource&) = delete;
    SymbolSource(SymbolSource&&) = delete;
    SymbolSource& operator=(SymbolSource&&) = delete;

    [[nodiscard]] virtual std::optional<uint64_t> FindExport(std::string_view module, std::string_view symbol) = 0;
    [[nodiscard]] virtual std::optional<uint64_t> FindExport(std::string_view module, uint16_t ordinal) = 0;
};

// Optional sources, consulted as: title module, shim, host, then a stub.
struct ImportSources
{
    SymbolSource* TitleModules = nullptr;
    SymbolSource* Host = nullptr;
};

enum class ResolveError : uint8_t
{
    None,
    BadImportTable,
    StubAllocationFailed,
    ProtectionFailed,
    OutOfMemory,
};

[[nodiscard]] std::string_view ToString(ResolveError error) noexcept;

class ImportStubs;

// Result of binding an image. Owns the generated stubs, so it must outlive
// any use of the image's import address tables.
class BoundImports
{
public:
    BoundImports() noexcept;
    ~BoundImports();
    BoundImports(BoundImports&&) noexcept;
    BoundImports& operator=(BoundImports&&) noexcept;
    BoundImports(const BoundImports&) = delete;
    BoundImports& operator=(const BoundImports&) = delete;

    std::vector<ImportBinding> Bindings;
    std::unique_ptr<ImportStubs> Stubs;

    [[nodiscard]] size_t StubbedCount() const noexcept;
    [[nodiscard]] size_t CountFrom(ImportSource source) const noexcept;
};

struct ResolveResult
{
    ResolveError Error = ResolveError::None;
    std::optional<BoundImports> Imports; // Set exactly when Error == ResolveError::None.
};

// Fills the import address tables (regular and delay-load, bound eagerly) of a
// mapped image. Each import is looked up in title modules, then `shims`, then
// host DLLs; anything left is bound to a generated stub that records each call
// in `stubs` and returns DefaultStubReturnValue. Final page protections are
// restored afterwards.
[[nodiscard]] ResolveResult ResolveImports(const MappedImage& image, const PeImage& pe,
                                           const Kernel::ShimRegistry& shims, Kernel::StubRegistry& stubs,
                                           ImageAllocator& stubAllocator, Core::Logger& logger,
                                           ImportSources sources) noexcept;
[[nodiscard]] ResolveResult ResolveImports(const MappedImage& image, const PeImage& pe,
                                           const Kernel::ShimRegistry& shims, Kernel::StubRegistry& stubs,
                                           ImageAllocator& stubAllocator, Core::Logger& logger) noexcept;
} // namespace OpenEra::Loader
