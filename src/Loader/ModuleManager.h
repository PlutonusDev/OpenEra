#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"
#include "Loader/HostModules.h"
#include "Loader/ImageMapper.h"
#include "Loader/ImageMemory.h"
#include "Loader/ImportResolver.h"
#include "Loader/PeReader.h"
#include "Loader/UnwindRegistration.h"

namespace OpenEra::Core
{
class Logger;
} // namespace OpenEra::Core

namespace OpenEra::Loader
{
// Returns a title module's bytes by plain file name, or nullopt if the title doesn't ship it.
using ModuleReader = std::function<std::optional<std::vector<std::byte>>(std::string_view fileName)>;

// Reads modules from one directory (the title's package root). Names with a
// path, or reserved system module names, are never read.
[[nodiscard]] ModuleReader DirectoryModuleReader(std::filesystem::path directory);

// System module names (Win32, the C runtime, graphics, audio, input, GDK and
// API sets) that are never loaded from a title package, whatever files it contains.
[[nodiscard]] bool IsReservedModuleName(std::string_view module);

struct LoadedModule
{
    std::string Name; // File name it was loaded as.
    PeImage Pe;
    MappedImage Image;
    UnwindTable Unwind; // Declared after Image, so it is unregistered first.
    std::optional<BoundImports> Imports;
    std::vector<size_t> Dependencies; // Title modules it imports from.
    std::unordered_map<std::string, size_t> ExportsByName;
    std::unordered_map<uint32_t, size_t> ExportsByOrdinal;
    bool Attached = false;

    [[nodiscard]] uint64_t Base() const noexcept { return Image.Base(); }
};

// Runs a module's start-up and shutdown (TLS callbacks and DllMain: #15).
// The manager decides the order.
class ModuleInitializer
{
public:
    ModuleInitializer() = default;
    virtual ~ModuleInitializer() = default;
    ModuleInitializer(const ModuleInitializer&) = delete;
    ModuleInitializer& operator=(const ModuleInitializer&) = delete;
    ModuleInitializer(ModuleInitializer&&) = delete;
    ModuleInitializer& operator=(ModuleInitializer&&) = delete;

    [[nodiscard]] virtual bool Attach(const LoadedModule& module) = 0;
    virtual void Detach(const LoadedModule& module) = 0;
};

enum class ModuleError : uint8_t
{
    None,
    NotFound,
    Reserved,
    BadImage,
    MapFailed,
    LinkFailed,
    AttachFailed,
    TooDeep,
    OutOfMemory,
};

[[nodiscard]] std::string_view ToString(ModuleError error) noexcept;

// Owns every image loaded from the title package. Loading maps a module and its
// title dependencies first, then links them (title modules, shims, host
// passthrough, stubs), then attaches new modules dependencies-first. Import
// cycles are fine: everything is mapped before anything is linked.
class ModuleManager final : public SymbolSource
{
public:
    ModuleManager(ModuleReader reader, const Kernel::ShimRegistry& shims, Kernel::StubRegistry& stubs,
                  ImageAllocator& allocator, Core::Logger& logger, HostModules* host);

    [[nodiscard]] ModuleError Load(std::string_view name);
    void SetInitializer(ModuleInitializer* initializer) noexcept;
    // Detaches attached modules in reverse attach order (process exit).
    void Shutdown();

    [[nodiscard]] const LoadedModule* Find(std::string_view name) const;
    [[nodiscard]] const LoadedModule* FindByHandle(uint64_t handle) const;
    [[nodiscard]] std::vector<std::string> AttachOrder() const;
    [[nodiscard]] size_t ModuleCount() const;
    // Base of the title module containing `address`, or 0.
    [[nodiscard]] uint64_t ImageBaseFor(uint64_t address) const;

    // Exports of loaded title modules, following forwarders.
    [[nodiscard]] std::optional<uint64_t> FindExport(std::string_view module, std::string_view symbol) override;
    [[nodiscard]] std::optional<uint64_t> FindExport(std::string_view module, uint16_t ordinal) override;

    // Runtime loader (LoadLibrary, GetModuleHandle, GetProcAddress). Handles are
    // image bases for title modules, real HMODULEs for host modules, and
    // opaque pseudo-handles for shim-only modules. 0 means not found.
    [[nodiscard]] uint64_t LoadForRuntime(std::string_view name);
    [[nodiscard]] uint64_t HandleOf(std::string_view name);
    [[nodiscard]] uint64_t MainModuleHandle() const;
    [[nodiscard]] uint64_t ProcAddress(uint64_t handle, std::string_view symbol);
    [[nodiscard]] uint64_t ProcAddress(uint64_t handle, uint16_t ordinal);

    // Manager used by the loader shims (RegisterLoaderShims).
    static void SetActive(ModuleManager* manager) noexcept;
    [[nodiscard]] static ModuleManager* Active() noexcept;

private:
    struct MapOutcome
    {
        ModuleError Error = ModuleError::None;
        size_t Index = 0;
    };

    MapOutcome MapTree(std::string_view name, size_t depth, std::vector<size_t>& fresh);
    ModuleError Attach(size_t root);
    [[nodiscard]] const LoadedModule* FindLocked(std::string_view name) const;
    [[nodiscard]] std::optional<uint64_t> ExportAddress(size_t module, size_t exportIndex, size_t depth);
    [[nodiscard]] std::optional<uint64_t> ResolveForwarder(std::string_view forwarder, size_t depth);
    [[nodiscard]] std::optional<uint64_t> FindInModule(size_t module, std::string_view symbol, size_t depth);
    [[nodiscard]] std::optional<uint64_t> FindInModule(size_t module, uint16_t ordinal, size_t depth);
    [[nodiscard]] uint64_t ShimHandle(std::string_view name);
    [[nodiscard]] uint64_t ProcFromHandle(uint64_t handle, std::string_view symbol, std::optional<uint16_t> ordinal);

    ModuleReader m_reader;
    std::reference_wrapper<const Kernel::ShimRegistry> m_shims;
    std::reference_wrapper<Kernel::StubRegistry> m_stubs;
    std::reference_wrapper<ImageAllocator> m_allocator;
    std::reference_wrapper<Core::Logger> m_logger;
    HostModules* m_host;
    ModuleInitializer* m_initializer = nullptr;

    mutable std::recursive_mutex m_mutex;
    std::deque<LoadedModule> m_modules; // Stable addresses.
    std::unordered_map<std::string, size_t> m_byKey;
    std::vector<size_t> m_attachOrder;
    std::unordered_map<uint64_t, const Kernel::ShimModule*> m_shimHandles;
    uint64_t m_mainHandle = 0;
};

// Registers the loader shims, backed by ModuleManager::Active():
// - kernel32.dll (aliases kernelbase and the libraryloader API sets):
//   LoadLibrary{A,W,ExA,ExW}, GetModuleHandle{A,W}, GetProcAddress,
//   FreeLibrary and RtlPcToFileHeader, plus the file (Kernel::AddFileExports)
//   and memory (Kernel::AddMemoryExports) shims, also under the file and
//   memory API sets, and OutputDebugString{W,A} into the log
//   (Kernel::AddDebugOutputExports), and thread affinity, priority and
//   naming (Kernel::AddThreadExports), also under the processthreads API sets.
// - ntdll.dll (aliases the rtlsupport API sets): RtlPcToFileHeader.
// - vcruntime140.dll: _CxxThrowException, which records the thrower's image
//   base for title modules the OS loader doesn't know.
// Other exports of these modules fall through to host passthrough.
bool RegisterLoaderShims(Kernel::ShimRegistry& shims);
} // namespace OpenEra::Loader
