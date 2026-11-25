#include "ShimModule.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace OpenEra::Kernel
{
namespace
{
constexpr char ToLowerAscii(char character) noexcept
{
    if (character >= 'A' && character <= 'Z')
    {
        character += 'a' - 'A';
    }
    return character;
}

constexpr char Identity(char character) noexcept
{
    return character;
}

// FNV-1a over the transformed characters.
template <typename Transform>
size_t Fnv1a(std::string_view text, Transform transform) noexcept
{
    uint64_t hash = 0xCBF2'9CE4'8422'2325ULL;
    for (const char character : text)
    {
        hash ^= std::bit_cast<unsigned char>(transform(character));
        hash *= 0x0000'0100'0000'01B3ULL;
    }
    return hash;
}
} // namespace

namespace Detail
{
size_t CaseInsensitiveHash::operator()(std::string_view text) const noexcept
{
    return Fnv1a(NormalizeModuleName(text), ToLowerAscii);
}

bool CaseInsensitiveEqual::operator()(std::string_view left, std::string_view right) const noexcept
{
    return std::ranges::equal(NormalizeModuleName(left), NormalizeModuleName(right), {}, ToLowerAscii, ToLowerAscii);
}

size_t StringHash::operator()(std::string_view text) const noexcept
{
    return Fnv1a(text, Identity);
}
} // namespace Detail

std::string_view NormalizeModuleName(std::string_view name) noexcept
{
    constexpr std::string_view kExtension = ".dll";
    if (name.size() > kExtension.size() &&
        std::ranges::equal(name.substr(name.size() - kExtension.size()), kExtension, {}, ToLowerAscii))
    {
        name.remove_suffix(kExtension.size());
    }
    return name;
}

ShimModule::ShimModule(std::string_view name, StubKind kind)
    : m_name(name)
    , m_kind(kind)
{
}

bool ShimModule::Export(std::string_view name, ShimProc proc, std::optional<uint16_t> ordinal)
{
    if (name.empty() || m_byName.contains(name) || (ordinal && m_byOrdinal.contains(*ordinal)))
    {
        return false;
    }

    const auto& added = m_exports.emplace_back(ShimExport{.Name = std::string(name), .Ordinal = ordinal, .Proc = proc});
    m_byName.emplace(added.Name, &added);
    if (ordinal)
    {
        m_byOrdinal.emplace(*ordinal, &added);
    }
    return true;
}

bool ShimModule::ExportOrdinal(uint16_t ordinal, ShimProc proc)
{
    if (m_byOrdinal.contains(ordinal))
    {
        return false;
    }

    const auto& added = m_exports.emplace_back(ShimExport{.Name = {}, .Ordinal = ordinal, .Proc = proc});
    m_byOrdinal.emplace(ordinal, &added);
    return true;
}

void ShimModule::AddAlias(std::string_view alias)
{
    m_aliases.emplace_back(alias);
}

const ShimExport* ShimModule::Find(std::string_view name) const
{
    const auto it = m_byName.find(name);
    return it == m_byName.end() ? nullptr : it->second;
}

const ShimExport* ShimModule::Find(uint16_t ordinal) const
{
    const auto it = m_byOrdinal.find(ordinal);
    return it == m_byOrdinal.end() ? nullptr : it->second;
}

bool ShimRegistry::Register(ShimModule module)
{
    if (m_byName.contains(module.Name()) ||
        std::ranges::any_of(module.Aliases(), [this](const std::string& alias) { return m_byName.contains(alias); }))
    {
        return false;
    }

    const auto& added = m_modules.emplace_back(std::move(module));
    m_byName.emplace(added.Name(), &added);
    for (const auto& alias : added.Aliases())
    {
        m_byName.emplace(alias, &added);
    }
    return true;
}

const ShimModule* ShimRegistry::FindModule(std::string_view name) const
{
    const auto it = m_byName.find(name);
    return it == m_byName.end() ? nullptr : it->second;
}

std::optional<ShimResolution> ShimRegistry::Resolve(std::string_view module, std::string_view symbol) const
{
    const auto* shimModule = FindModule(module);
    const auto* shimExport = shimModule != nullptr ? shimModule->Find(symbol) : nullptr;
    if (shimExport == nullptr)
    {
        return std::nullopt;
    }
    return ShimResolution{.Module = shimModule, .Export = shimExport};
}

std::optional<ShimResolution> ShimRegistry::Resolve(std::string_view module, uint16_t ordinal) const
{
    const auto* shimModule = FindModule(module);
    const auto* shimExport = shimModule != nullptr ? shimModule->Find(ordinal) : nullptr;
    if (shimExport == nullptr)
    {
        return std::nullopt;
    }
    return ShimResolution{.Module = shimModule, .Export = shimExport};
}

ShimRegistry& DefaultShimRegistry()
{
    static ShimRegistry registry;
    return registry;
}
} // namespace OpenEra::Kernel
