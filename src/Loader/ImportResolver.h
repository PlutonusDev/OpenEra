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

struct ImportBinding
{
    std::string Module;
    std::string Symbol; // Empty when imported by ordinal.
    std::optional<uint16_t> Ordinal;
    uint64_t Address = 0;
    bool Stubbed = false;
    bool Delayed = false;
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
};

struct ResolveResult
{
    ResolveError Error = ResolveError::None;
    std::optional<BoundImports> Imports; // Set exactly when Error == ResolveError::None.
};

// Fills the import address tables (regular and delay-load, bound eagerly) of a
// mapped image from `shims`. Imports with no shim are bound to generated stubs
// that record each call in `stubs` and return DefaultStubReturnValue. Final
// page protections are restored afterwards.
[[nodiscard]] ResolveResult ResolveImports(const MappedImage& image, const PeImage& pe,
                                           const Kernel::ShimRegistry& shims, Kernel::StubRegistry& stubs,
                                           ImageAllocator& stubAllocator, Core::Logger& logger) noexcept;
} // namespace OpenEra::Loader
