#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Loader/ImportResolver.h"

namespace OpenEra::Loader
{
namespace Detail
{
struct LibraryDeleter
{
    void operator()(void* module) const noexcept; // FreeLibrary
};
} // namespace Detail

// Passthrough to the host's own Windows DLLs, for modules where desktop
// Windows already behaves correctly (Win32, the C runtime, Winsock).
//
// Only plain module names on the allowlist are loaded, and only from System32
// (LOAD_LIBRARY_SEARCH_SYSTEM32). Names with a path are refused, so a title can
// never get a DLL from anywhere else loaded through this, and Xbox system DLLs
// from a console dump are never touched.
class HostModules final : public SymbolSource
{
public:
    HostModules(const std::vector<std::string>& modules, const std::vector<std::string>& prefixes);

    // With the default allowlist (DefaultModules / DefaultPrefixes).
    [[nodiscard]] static std::unique_ptr<HostModules> CreateDefault();

    [[nodiscard]] static std::vector<std::string> DefaultModules();
    [[nodiscard]] static std::vector<std::string> DefaultPrefixes();

    // ASCII case-insensitive, with or without ".dll". False for any name with a path.
    [[nodiscard]] bool IsAllowed(std::string_view module) const;

    // Module handle (HMODULE as an integer), loading the module on first use.
    [[nodiscard]] std::optional<uint64_t> Handle(std::string_view module);
    // Module key (see ModuleKey) a handle returned by Handle belongs to.
    [[nodiscard]] std::optional<std::string> ModuleForHandle(uint64_t handle) const;

    [[nodiscard]] std::optional<uint64_t> FindExport(std::string_view module, std::string_view symbol) override;
    [[nodiscard]] std::optional<uint64_t> FindExport(std::string_view module, uint16_t ordinal) override;

private:
    [[nodiscard]] std::optional<uint64_t> Lookup(std::string_view module, const char* nameOrOrdinal);

    std::vector<std::string> m_modules;  // Keys.
    std::vector<std::string> m_prefixes; // Lowercase.
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::unique_ptr<void, Detail::LibraryDeleter>> m_loaded;
};

// Lowercase module name without a trailing ".dll", for comparisons.
[[nodiscard]] std::string ModuleKey(std::string_view module);
} // namespace OpenEra::Loader
