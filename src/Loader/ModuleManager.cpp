#include "ModuleManager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <fstream>
#include <iterator>
#include <new>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "Core/Logger.h"

namespace OpenEra::Loader
{
namespace
{
using Core::Logger;

constexpr size_t kMaxLoadDepth = 64;
constexpr size_t kMaxForwarderDepth = 16;
constexpr std::string_view kMissNote = "not found by GetProcAddress";

std::atomic<ModuleManager*>& ActiveManager() noexcept
{
    static std::atomic<ModuleManager*> active{nullptr};
    return active;
}

// Drops any directory part, so runtime loads by path find the package module.
std::string_view FileNameOnly(std::string_view name) noexcept
{
    const auto slash = name.find_last_of("\\/");
    return slash == std::string_view::npos ? name : name.substr(slash + 1);
}

std::string FileNameFor(std::string_view name)
{
    std::string file(FileNameOnly(name));
    if (file.find('.') == std::string::npos)
    {
        file += ".dll";
    }
    return file;
}

std::optional<uint16_t> ParseOrdinal(std::string_view text) noexcept
{
    if (!text.starts_with('#'))
    {
        return std::nullopt;
    }
    text.remove_prefix(1);
    if (text.empty() || text.size() > 5)
    {
        return std::nullopt;
    }
    uint32_t value = 0;
    for (const char digit : text)
    {
        if (digit < '0' || digit > '9')
        {
            return std::nullopt;
        }
        const uint32_t digitValue = std::bit_cast<unsigned char>(digit) - uint32_t{'0'};
        value = (value * 10) + digitValue;
    }
    if (value > 0xFFFF)
    {
        return std::nullopt;
    }
    return std::bit_cast<std::array<uint16_t, 2>>(value).front();
}
} // namespace

bool IsReservedModuleName(std::string_view module)
{
    const std::string key = ModuleKey(FileNameOnly(module));
    static const std::vector<std::string> kExtra = {"ntdll",     "combase",   "crypt32",  "winhttp",   "xgameruntime",
                                                    "d3d12",     "d3d12_x",   "dxgi",     "gameinput", "xaudio2_9",
                                                    "xaudio2_8", "xinput1_4", "xapobase", "xmem",      "xboxkrnl"};
    const auto defaults = HostModules::DefaultModules();
    return key.starts_with("api-ms-win-") || key.starts_with("ext-ms-win-") ||
           std::ranges::find(defaults, key) != defaults.end() || std::ranges::find(kExtra, key) != kExtra.end();
}

ModuleReader DirectoryModuleReader(std::filesystem::path directory)
{
    return [directory = std::move(directory)](std::string_view fileName) -> std::optional<std::vector<std::byte>>
    {
        if (fileName.empty() || fileName.find_first_of("\\/:") != std::string_view::npos ||
            fileName.find("..") != std::string_view::npos || IsReservedModuleName(fileName))
        {
            return std::nullopt;
        }
        std::ifstream stream(directory / std::filesystem::path(std::string(fileName)), std::ios::binary);
        if (!stream)
        {
            return std::nullopt;
        }
        const std::vector<char> raw((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(raw.size());
        std::ranges::transform(raw, bytes.begin(), [](char value) { return std::bit_cast<std::byte>(value); });
        return bytes;
    };
}

std::string_view ToString(ModuleError error) noexcept
{
    switch (error)
    {
    case ModuleError::None:
        return "None";
    case ModuleError::NotFound:
        return "NotFound";
    case ModuleError::Reserved:
        return "Reserved";
    case ModuleError::BadImage:
        return "BadImage";
    case ModuleError::MapFailed:
        return "MapFailed";
    case ModuleError::LinkFailed:
        return "LinkFailed";
    case ModuleError::AttachFailed:
        return "AttachFailed";
    case ModuleError::TooDeep:
        return "TooDeep";
    case ModuleError::OutOfMemory:
        return "OutOfMemory";
    }
    return "Unknown";
}

ModuleManager::ModuleManager(ModuleReader reader, const Kernel::ShimRegistry& shims, Kernel::StubRegistry& stubs,
                             ImageAllocator& allocator, Core::Logger& logger, HostModules* host)
    : m_reader(std::move(reader))
    , m_shims(shims)
    , m_stubs(stubs)
    , m_allocator(allocator)
    , m_logger(logger)
    , m_host(host)
{
}

void ModuleManager::SetInitializer(ModuleInitializer* initializer) noexcept
{
    m_initializer = initializer;
}

void ModuleManager::SetActive(ModuleManager* manager) noexcept
{
    ActiveManager().store(manager);
}

ModuleManager* ModuleManager::Active() noexcept
{
    return ActiveManager().load();
}

ModuleError ModuleManager::Load(std::string_view name)
{
    const std::scoped_lock lock(m_mutex);
    if (IsReservedModuleName(name))
    {
        m_logger.get().Warn("Modules: {} is a system module name and is never loaded from the title", name);
        return ModuleError::Reserved;
    }

    std::vector<size_t> fresh;
    const auto outcome = MapTree(name, 0, fresh);
    if (outcome.Error != ModuleError::None)
    {
        return outcome.Error;
    }
    if (m_mainHandle == 0)
    {
        m_mainHandle = m_modules.at(outcome.Index).Base();
    }

    for (const size_t index : fresh)
    {
        auto& module = m_modules.at(index);
        auto result = ResolveImports(module.Image, module.Pe, m_shims.get(), m_stubs.get(), m_allocator.get(),
                                     m_logger.get(), ImportSources{.TitleModules = this, .Host = m_host});
        if (result.Error != ResolveError::None)
        {
            m_logger.get().Warn("Modules: linking {} failed ({})", module.Name, ToString(result.Error));
            return ModuleError::LinkFailed;
        }
        module.Imports = std::move(result.Imports);
    }
    return Attach(outcome.Index);
}

ModuleManager::MapOutcome ModuleManager::MapTree(std::string_view name, size_t depth, std::vector<size_t>& fresh)
{
    if (depth > kMaxLoadDepth)
    {
        m_logger.get().Warn("Modules: dependency chain deeper than {} at {}", kMaxLoadDepth, name);
        return {.Error = ModuleError::TooDeep, .Index = 0};
    }

    const std::string key = ModuleKey(FileNameOnly(name));
    if (const auto it = m_byKey.find(key); it != m_byKey.end())
    {
        return {.Error = ModuleError::None, .Index = it->second};
    }
    if (IsReservedModuleName(name))
    {
        return {.Error = ModuleError::NotFound, .Index = 0};
    }

    const std::string fileName = FileNameFor(name);
    const auto bytes = m_reader(fileName);
    if (!bytes)
    {
        return {.Error = ModuleError::NotFound, .Index = 0};
    }

    auto parsed = ParsePe(*bytes, m_logger.get());
    if (!parsed.Image)
    {
        m_logger.get().Warn("Modules: {} is not a valid PE32+ image ({})", fileName, ToString(parsed.Error));
        return {.Error = ModuleError::BadImage, .Index = 0};
    }
    auto mapped = MapImage(*bytes, *parsed.Image, m_allocator.get(), m_logger.get());
    if (!mapped.Image)
    {
        m_logger.get().Warn("Modules: mapping {} failed ({})", fileName, ToString(mapped.Error));
        return {.Error = ModuleError::MapFailed, .Index = 0};
    }
    auto unwind = RegisterUnwindTable(*parsed.Image, *mapped.Image->Memory);
    if (!unwind)
    {
        m_logger.get().Warn("Modules: registering unwind data for {} failed", fileName);
        return {.Error = ModuleError::MapFailed, .Index = 0};
    }

    const size_t index = m_modules.size();
    auto& module = m_modules.emplace_back();
    module.Name = fileName;
    module.Pe = std::move(*parsed.Image);
    module.Image = std::move(*mapped.Image);
    module.Unwind = std::move(*unwind);
    for (size_t exportIndex = 0; exportIndex < module.Pe.Exports.size(); ++exportIndex)
    {
        const auto& entry = module.Pe.Exports.at(exportIndex);
        if (!entry.Name.empty())
        {
            module.ExportsByName.emplace(entry.Name, exportIndex);
        }
        module.ExportsByOrdinal.emplace(entry.Ordinal, exportIndex);
    }
    m_byKey.emplace(key, index);
    fresh.push_back(index);
    m_logger.get().Info("Modules: mapped {} at {:#x}", fileName, module.Base());

    std::vector<std::string> dependencies;
    dependencies.reserve(module.Pe.Imports.size() + module.Pe.DelayImports.size());
    for (const auto& imported : module.Pe.Imports)
    {
        dependencies.push_back(imported.DllName);
    }
    for (const auto& imported : module.Pe.DelayImports)
    {
        dependencies.push_back(imported.DllName);
    }
    for (const auto& dependency : dependencies)
    {
        const auto outcome = MapTree(dependency, depth + 1, fresh);
        if (outcome.Error == ModuleError::NotFound)
        {
            continue; // Not a title module: shims, host or stubs will cover it.
        }
        if (outcome.Error != ModuleError::None)
        {
            return outcome;
        }
        auto& owner = m_modules.at(index);
        if (outcome.Index != index && std::ranges::find(owner.Dependencies, outcome.Index) == owner.Dependencies.end())
        {
            owner.Dependencies.push_back(outcome.Index);
        }
    }
    return {.Error = ModuleError::None, .Index = index};
}

ModuleError ModuleManager::Attach(size_t root)
{
    // Dependencies first; a module already on the current path (a cycle) is skipped.
    std::unordered_set<size_t> visiting;
    std::vector<std::pair<size_t, size_t>> stack{{root, 0}};
    visiting.insert(root);
    while (!stack.empty())
    {
        auto& [index, next] = stack.back();
        const auto& module = m_modules.at(index);
        if (next < module.Dependencies.size())
        {
            const size_t dependency = module.Dependencies.at(next++);
            if (!m_modules.at(dependency).Attached && visiting.insert(dependency).second)
            {
                stack.emplace_back(dependency, 0);
            }
            continue;
        }

        auto& current = m_modules.at(index);
        if (!current.Attached)
        {
            if (m_initializer != nullptr && !m_initializer->Attach(current))
            {
                m_logger.get().Warn("Modules: initialising {} failed", current.Name);
                return ModuleError::AttachFailed;
            }
            current.Attached = true;
            m_attachOrder.push_back(index);
        }
        stack.pop_back();
    }
    return ModuleError::None;
}

void ModuleManager::Shutdown()
{
    const std::scoped_lock lock(m_mutex);
    for (auto it = m_attachOrder.rbegin(); it != m_attachOrder.rend(); ++it)
    {
        auto& module = m_modules.at(*it);
        if (m_initializer != nullptr)
        {
            m_initializer->Detach(module);
        }
        module.Attached = false;
    }
    m_attachOrder.clear();
}

const LoadedModule* ModuleManager::FindLocked(std::string_view name) const
{
    const auto it = m_byKey.find(ModuleKey(FileNameOnly(name)));
    return it == m_byKey.end() ? nullptr : &m_modules.at(it->second);
}

const LoadedModule* ModuleManager::Find(std::string_view name) const
{
    const std::scoped_lock lock(m_mutex);
    return FindLocked(name);
}

const LoadedModule* ModuleManager::FindByHandle(uint64_t handle) const
{
    const std::scoped_lock lock(m_mutex);
    const auto it = std::ranges::find_if(m_modules, [handle](const LoadedModule& module) noexcept
                                         { return module.Base() == handle; });
    return it == m_modules.end() ? nullptr : &*it;
}

std::vector<std::string> ModuleManager::AttachOrder() const
{
    const std::scoped_lock lock(m_mutex);
    std::vector<std::string> names;
    names.reserve(m_attachOrder.size());
    for (const size_t index : m_attachOrder)
    {
        names.push_back(m_modules.at(index).Name);
    }
    return names;
}

size_t ModuleManager::ModuleCount() const
{
    const std::scoped_lock lock(m_mutex);
    return m_modules.size();
}

uint64_t ModuleManager::ImageBaseFor(uint64_t address) const
{
    const std::scoped_lock lock(m_mutex);
    const auto it =
        std::ranges::find_if(m_modules,
                             [address](const LoadedModule& module) noexcept
                             {
                                 const uint64_t base = module.Base();
                                 return address >= base && address - base < module.Pe.OptionalHeader.SizeOfImage;
                             });
    return it == m_modules.end() ? 0 : it->Base();
}

std::optional<uint64_t> ModuleManager::ExportAddress(size_t module, size_t exportIndex, size_t depth)
{
    const auto& loaded = m_modules.at(module);
    const auto& entry = loaded.Pe.Exports.at(exportIndex);
    if (entry.Forwarder.empty())
    {
        return loaded.Base() + entry.Rva;
    }
    return ResolveForwarder(entry.Forwarder, depth + 1);
}

std::optional<uint64_t> ModuleManager::FindInModule(size_t module, std::string_view symbol, size_t depth)
{
    const auto& exports = m_modules.at(module).ExportsByName;
    const auto it = exports.find(std::string(symbol));
    return it == exports.end() ? std::nullopt : ExportAddress(module, it->second, depth);
}

std::optional<uint64_t> ModuleManager::FindInModule(size_t module, uint16_t ordinal, size_t depth)
{
    const auto& exports = m_modules.at(module).ExportsByOrdinal;
    const auto it = exports.find(ordinal);
    return it == exports.end() ? std::nullopt : ExportAddress(module, it->second, depth);
}

std::optional<uint64_t> ModuleManager::ResolveForwarder(std::string_view forwarder, size_t depth)
{
    if (depth > kMaxForwarderDepth)
    {
        m_logger.get().Warn("Modules: forwarder chain deeper than {} at {}", kMaxForwarderDepth, forwarder);
        return std::nullopt;
    }
    const auto dot = forwarder.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == forwarder.size())
    {
        m_logger.get().Warn("Modules: malformed forwarder {}", forwarder);
        return std::nullopt;
    }
    const auto module = forwarder.substr(0, dot);
    const auto symbol = forwarder.substr(dot + 1);
    const auto ordinal = ParseOrdinal(symbol);

    if (const auto it = m_byKey.find(ModuleKey(module)); it != m_byKey.end())
    {
        return ordinal ? FindInModule(it->second, *ordinal, depth) : FindInModule(it->second, symbol, depth);
    }
    const auto shim = ordinal ? m_shims.get().Resolve(module, *ordinal) : m_shims.get().Resolve(module, symbol);
    if (shim)
    {
        return std::bit_cast<uint64_t>(shim->Export->Proc);
    }
    if (m_host != nullptr)
    {
        return ordinal ? m_host->FindExport(module, *ordinal) : m_host->FindExport(module, symbol);
    }
    return std::nullopt;
}

std::optional<uint64_t> ModuleManager::FindExport(std::string_view module, std::string_view symbol)
{
    const std::scoped_lock lock(m_mutex);
    const auto it = m_byKey.find(ModuleKey(FileNameOnly(module)));
    return it == m_byKey.end() ? std::nullopt : FindInModule(it->second, symbol, 0);
}

std::optional<uint64_t> ModuleManager::FindExport(std::string_view module, uint16_t ordinal)
{
    const std::scoped_lock lock(m_mutex);
    const auto it = m_byKey.find(ModuleKey(FileNameOnly(module)));
    return it == m_byKey.end() ? std::nullopt : FindInModule(it->second, ordinal, 0);
}

uint64_t ModuleManager::ShimHandle(std::string_view name)
{
    const auto* shimModule = m_shims.get().FindModule(name);
    if (shimModule == nullptr)
    {
        return 0;
    }
    const auto handle = std::bit_cast<uint64_t>(shimModule);
    m_shimHandles.emplace(handle, shimModule);
    return handle;
}

uint64_t ModuleManager::HandleOf(std::string_view name)
{
    const std::scoped_lock lock(m_mutex);
    const auto file = FileNameOnly(name);
    if (const auto* module = FindLocked(file))
    {
        return module->Base();
    }
    if (m_host != nullptr && m_host->IsAllowed(file))
    {
        return m_host->Handle(file).value_or(0);
    }
    return ShimHandle(file);
}

uint64_t ModuleManager::LoadForRuntime(std::string_view name)
{
    const std::scoped_lock lock(m_mutex);
    const auto file = FileNameOnly(name);
    if (file.empty())
    {
        return 0;
    }
    if (const auto handle = HandleOf(file); handle != 0)
    {
        return handle;
    }
    if (IsReservedModuleName(file) || Load(file) != ModuleError::None)
    {
        return 0;
    }
    const auto* module = FindLocked(file);
    return module == nullptr ? 0 : module->Base();
}

void ModuleManager::SetImageDirectory(const std::filesystem::path& directory)
{
    const std::scoped_lock lock(m_mutex);
    std::error_code error;
    m_imageDirectory = std::filesystem::absolute(directory, error).lexically_normal();
}

std::optional<std::filesystem::path> ModuleManager::ImagePath(uint64_t handle) const
{
    const std::scoped_lock lock(m_mutex);
    const uint64_t base = handle == 0 ? m_mainHandle : handle;
    if (m_imageDirectory.empty() || base == 0)
    {
        return std::nullopt;
    }
    const auto it =
        std::ranges::find_if(m_modules, [base](const LoadedModule& module) noexcept { return module.Base() == base; });
    if (it == m_modules.end())
    {
        return std::nullopt;
    }
    return m_imageDirectory / std::filesystem::path(std::u8string(it->Name.begin(), it->Name.end()));
}

uint64_t ModuleManager::MainModuleHandle() const
{
    const std::scoped_lock lock(m_mutex);
    return m_mainHandle;
}

uint64_t ModuleManager::ProcFromHandle(uint64_t handle, std::string_view symbol, std::optional<uint16_t> ordinal)
{
    const auto lookup = [&](auto& source, std::string_view module) -> std::optional<uint64_t>
    { return ordinal ? source.FindExport(module, *ordinal) : source.FindExport(module, symbol); };

    std::string moduleName;
    std::optional<uint64_t> address;
    if (const auto* module = FindByHandle(handle))
    {
        moduleName = module->Name;
        const auto index = m_byKey.at(ModuleKey(module->Name));
        address = ordinal ? FindInModule(index, *ordinal, 0) : FindInModule(index, symbol, 0);
    }
    else if (const auto shim = m_shimHandles.find(handle); shim != m_shimHandles.end())
    {
        moduleName = shim->second->Name();
        const auto* exported = ordinal ? shim->second->Find(*ordinal) : shim->second->Find(symbol);
        if (exported != nullptr)
        {
            address = std::bit_cast<uint64_t>(exported->Proc);
        }
        else if (m_host != nullptr && m_host->IsAllowed(moduleName))
        {
            address = lookup(*m_host, moduleName);
        }
    }
    else if (m_host != nullptr)
    {
        if (const auto hostModule = m_host->ModuleForHandle(handle))
        {
            moduleName = *hostModule;
            // OpenEra shims override host exports here too (e.g. GetProcAddress itself).
            const auto overriding =
                ordinal ? m_shims.get().Resolve(moduleName, *ordinal) : m_shims.get().Resolve(moduleName, symbol);
            address = overriding ? std::optional(std::bit_cast<uint64_t>(overriding->Export->Proc))
                                 : lookup(*m_host, moduleName);
        }
    }

    if (!address && !moduleName.empty())
    {
        if (ordinal)
        {
            const uint32_t ordinalValue = *ordinal;
            m_stubs.get().RecordHit(Kernel::StubKind::Win32Import, moduleName, ordinalValue, kMissNote);
        }
        else
        {
            m_stubs.get().RecordHit(Kernel::StubKind::Win32Import, moduleName, symbol, kMissNote);
        }
    }
    return address.value_or(0);
}

uint64_t ModuleManager::ProcAddress(uint64_t handle, std::string_view symbol)
{
    const std::scoped_lock lock(m_mutex);
    return symbol.empty() ? 0 : ProcFromHandle(handle, symbol, std::nullopt);
}

uint64_t ModuleManager::ProcAddress(uint64_t handle, uint16_t ordinal)
{
    const std::scoped_lock lock(m_mutex);
    return ProcFromHandle(handle, {}, ordinal);
}
} // namespace OpenEra::Loader
