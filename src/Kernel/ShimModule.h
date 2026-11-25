#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Kernel/StubRegistry.h"

namespace OpenEra::Kernel
{
// Type-erased export address. Convert with ToShimProc / FromShimProc; round-tripping
// a function pointer through another function pointer type is well defined.
using ShimProc = void (*)();

template <typename Function>
[[nodiscard]] ShimProc ToShimProc(Function* function) noexcept
{
    return reinterpret_cast<ShimProc>(function);
}

template <typename Function>
[[nodiscard]] Function* FromShimProc(ShimProc proc) noexcept
{
    return reinterpret_cast<Function*>(proc);
}

struct ShimExport
{
    std::string Name; // Empty for ordinal-only exports.
    std::optional<uint16_t> Ordinal;
    ShimProc Proc = nullptr;
};

namespace Detail
{
// ASCII case-insensitive hashing and comparison, usable with std::string_view lookups.
struct CaseInsensitiveHash
{
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::string_view text) const noexcept;
};

struct CaseInsensitiveEqual
{
    using is_transparent = void;
    [[nodiscard]] bool operator()(std::string_view left, std::string_view right) const noexcept;
};

struct StringHash
{
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::string_view text) const noexcept;
};
} // namespace Detail

// A module that title imports can bind to, implemented by OpenEra.
// Symbol names are case-sensitive, as in PE export tables.
class ShimModule
{
public:
    ShimModule(std::string_view name, StubKind kind);
    ~ShimModule() = default;

    // Lookup tables point into m_exports: moving keeps them valid, copying would not.
    ShimModule(const ShimModule&) = delete;
    ShimModule& operator=(const ShimModule&) = delete;
    // MSVC's node-based containers may allocate when moved.
    ShimModule(ShimModule&&) noexcept(false) = default;
    ShimModule& operator=(ShimModule&&) noexcept(false) = default;

    // Return false, without changing the module, when the name or ordinal is already exported.
    bool Export(std::string_view name, ShimProc proc, std::optional<uint16_t> ordinal = std::nullopt);
    bool ExportOrdinal(uint16_t ordinal, ShimProc proc);
    // Another module name that resolves here, e.g. an API set contract.
    void AddAlias(std::string_view alias);

    [[nodiscard]] const ShimExport* Find(std::string_view name) const;
    [[nodiscard]] const ShimExport* Find(uint16_t ordinal) const;

    [[nodiscard]] const std::string& Name() const noexcept { return m_name; }
    [[nodiscard]] std::span<const std::string> Aliases() const noexcept { return m_aliases; }
    [[nodiscard]] StubKind Kind() const noexcept { return m_kind; }
    [[nodiscard]] size_t ExportCount() const noexcept { return m_exports.size(); }

private:
    std::string m_name;
    StubKind m_kind;
    std::vector<std::string> m_aliases;
    std::deque<ShimExport> m_exports; // Stable addresses for Find results.
    std::unordered_map<std::string, const ShimExport*, Detail::StringHash, std::equal_to<>> m_byName;
    std::unordered_map<uint16_t, const ShimExport*> m_byOrdinal;
};

struct ShimResolution
{
    const ShimModule* Module = nullptr;
    const ShimExport* Export = nullptr;
};

// Registered shim modules, looked up the way the loader names them: ASCII
// case-insensitive, with or without a ".dll" suffix. Register everything at
// startup; lookups are then safe from any thread.
class ShimRegistry
{
public:
    // Returns false, registering nothing, if the name or an alias is already taken.
    bool Register(ShimModule module);

    [[nodiscard]] const ShimModule* FindModule(std::string_view name) const;
    [[nodiscard]] std::optional<ShimResolution> Resolve(std::string_view module, std::string_view symbol) const;
    [[nodiscard]] std::optional<ShimResolution> Resolve(std::string_view module, uint16_t ordinal) const;

    [[nodiscard]] size_t ModuleCount() const noexcept { return m_modules.size(); }

private:
    std::deque<ShimModule> m_modules; // Stable addresses for FindModule results.
    std::unordered_map<std::string, const ShimModule*, Detail::CaseInsensitiveHash, Detail::CaseInsensitiveEqual>
        m_byName;
};

// Strips a trailing ".dll" (any case). API set version suffixes are kept.
[[nodiscard]] std::string_view NormalizeModuleName(std::string_view name) noexcept;

[[nodiscard]] ShimRegistry& DefaultShimRegistry();
} // namespace OpenEra::Kernel
