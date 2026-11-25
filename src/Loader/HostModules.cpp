#include "HostModules.h"

#include <algorithm>
#include <bit>

#include <Windows.h>

#include "Kernel/ShimModule.h"

namespace OpenEra::Loader
{
namespace
{
constexpr bool HasPath(std::string_view module) noexcept
{
    return module.find_first_of("\\/:") != std::string_view::npos || module.find("..") != std::string_view::npos;
}

std::vector<std::string> ToKeys(const std::vector<std::string>& names)
{
    std::vector<std::string> keys;
    keys.reserve(names.size());
    for (const auto& name : names)
    {
        keys.push_back(ModuleKey(name));
    }
    return keys;
}
} // namespace

void Detail::LibraryDeleter::operator()(void* module) const noexcept
{
    FreeLibrary(static_cast<HMODULE>(module));
}

std::string ModuleKey(std::string_view module)
{
    std::string key(Kernel::NormalizeModuleName(module));
    for (char& character : key)
    {
        if (character >= 'A' && character <= 'Z')
        {
            character += 'a' - 'A';
        }
    }
    return key;
}

std::unique_ptr<HostModules> HostModules::CreateDefault()
{
    return std::make_unique<HostModules>(DefaultModules(), DefaultPrefixes());
}

HostModules::HostModules(const std::vector<std::string>& modules, const std::vector<std::string>& prefixes)
    : m_modules(ToKeys(modules))
    , m_prefixes(ToKeys(prefixes))
{
}

std::vector<std::string> HostModules::DefaultModules()
{
    // Plain Win32, Winsock and the Visual C++ / Universal C runtimes. Graphics,
    // audio, input and GDK modules are deliberately absent: they need shims.
    return {"kernel32",
            "kernelbase",
            "user32",
            "gdi32",
            "advapi32",
            "ws2_32",
            "ole32",
            "oleaut32",
            "shell32",
            "shlwapi",
            "bcrypt",
            "ucrtbase",
            "vcruntime140",
            "vcruntime140_1",
            "msvcp140",
            "msvcp140_1",
            "msvcp140_2",
            "msvcp140_atomic_wait",
            "msvcp140_codecvt_ids"};
}

std::vector<std::string> HostModules::DefaultPrefixes()
{
    return {"api-ms-win-crt-", "api-ms-win-core-"};
}

bool HostModules::IsAllowed(std::string_view module) const
{
    if (module.empty() || HasPath(module))
    {
        return false;
    }
    const std::string key = ModuleKey(module);
    return std::ranges::find(m_modules, key) != m_modules.end() ||
           std::ranges::any_of(m_prefixes, [&key](const std::string& prefix) { return key.starts_with(prefix); });
}

std::optional<uint64_t> HostModules::Handle(std::string_view module)
{
    if (!IsAllowed(module) || !std::ranges::all_of(module, [](char character) { return character > 0; }))
    {
        return std::nullopt;
    }

    const std::string key = ModuleKey(module);
    const std::scoped_lock lock(m_mutex);
    if (const auto it = m_loaded.find(key); it != m_loaded.end())
    {
        return std::bit_cast<uint64_t>(it->second.get());
    }

    const std::wstring wide(key.begin(), key.end()); // ASCII only, checked above.
    HMODULE handle = LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (handle == nullptr)
    {
        return std::nullopt;
    }
    m_loaded.emplace(key, std::unique_ptr<void, Detail::LibraryDeleter>(handle));
    return std::bit_cast<uint64_t>(handle);
}

std::optional<std::string> HostModules::ModuleForHandle(uint64_t handle) const
{
    const std::scoped_lock lock(m_mutex);
    for (const auto& [key, loaded] : m_loaded)
    {
        if (std::bit_cast<uint64_t>(loaded.get()) == handle)
        {
            return key;
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> HostModules::Lookup(std::string_view module, const char* nameOrOrdinal)
{
    const auto handle = Handle(module);
    if (!handle)
    {
        return std::nullopt;
    }
    const auto address = std::bit_cast<uint64_t>(GetProcAddress(std::bit_cast<HMODULE>(*handle), nameOrOrdinal));
    if (address == 0)
    {
        return std::nullopt;
    }
    return address;
}

std::optional<uint64_t> HostModules::FindExport(std::string_view module, std::string_view symbol)
{
    if (symbol.empty())
    {
        return std::nullopt;
    }
    const std::string name(symbol);
    return Lookup(module, name.c_str());
}

std::optional<uint64_t> HostModules::FindExport(std::string_view module, uint16_t ordinal)
{
    // An ordinal is passed as a "name" whose high bits are zero (MAKEINTRESOURCEA).
    const uintptr_t ordinalName = ordinal;
    return Lookup(module, std::bit_cast<const char*>(ordinalName));
}
} // namespace OpenEra::Loader
