#include "TitleFileSystem.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace OpenEra::Kernel
{
namespace
{
constexpr std::wstring_view kDeletedFile = L".openera-deleted";
constexpr std::wstring_view kLongPathPrefix = L"\\\\?\\";

std::atomic<TitleFileSystem*>& ActiveFileSystem() noexcept
{
    static std::atomic<TitleFileSystem*> active{nullptr};
    return active;
}

constexpr wchar_t AsciiLower(wchar_t value) noexcept
{
    constexpr std::wstring_view kUpper = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    constexpr std::wstring_view kLower = L"abcdefghijklmnopqrstuvwxyz";
    const auto index = kUpper.find(value);
    return index == std::wstring_view::npos ? value : kLower.at(index);
}

bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) noexcept
{
    return std::ranges::equal(left, right,
                              [](wchar_t a, wchar_t b) noexcept { return AsciiLower(a) == AsciiLower(b); });
}

std::filesystem::path NormalizedRoot(const std::filesystem::path& root)
{
    std::error_code error;
    auto absolutePath = std::filesystem::absolute(root, error).lexically_normal();
    while (!absolutePath.has_filename() && absolutePath != absolutePath.root_path())
    {
        absolutePath = absolutePath.parent_path();
    }
    return absolutePath;
}

bool PathExists(const std::filesystem::path& path) noexcept
{
    std::error_code error;
    return std::filesystem::exists(path, error);
}
} // namespace

TitleFileSystem::TitleFileSystem(const std::filesystem::path& titleRoot, const std::filesystem::path& overlayRoot)
    : m_titleRoot(NormalizedRoot(titleRoot))
    , m_overlayRoot(NormalizedRoot(overlayRoot))
{
    std::error_code error;
    std::filesystem::create_directories(m_overlayRoot, error);
    std::ifstream stream(m_overlayRoot / kDeletedFile);
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty())
        {
            m_deleted.insert(std::filesystem::path(std::u8string(line.begin(), line.end())).wstring());
        }
    }
}

std::wstring TitleFileSystem::Key(const std::filesystem::path& relative)
{
    std::wstring key = relative.lexically_normal().generic_wstring();
    while (key.size() > 1 && key.ends_with(L'/'))
    {
        key.pop_back();
    }
    std::ranges::transform(key, key.begin(), AsciiLower);
    return key;
}

std::optional<std::filesystem::path> TitleFileSystem::TitleRelative(const std::filesystem::path& path) const
{
    std::wstring text = path.native();
    if (text.starts_with(kLongPathPrefix) && !text.starts_with(L"\\\\?\\UNC\\"))
    {
        text.erase(0, kLongPathPrefix.size());
    }
    std::error_code error;
    const auto absolutePath = std::filesystem::absolute(std::filesystem::path(text), error).lexically_normal();
    if (error)
    {
        return std::nullopt;
    }

    auto part = absolutePath.begin();
    for (const auto& rootPart : m_titleRoot)
    {
        if (part == absolutePath.end() || !EqualsIgnoreCase(rootPart.native(), part->native()))
        {
            return std::nullopt;
        }
        ++part;
    }
    std::filesystem::path relative;
    for (; part != absolutePath.end(); ++part)
    {
        if (!part->empty())
        {
            relative /= *part;
        }
    }
    return relative.empty() ? std::filesystem::path(L".") : relative;
}

bool TitleFileSystem::IsDeleted(const std::filesystem::path& relative) const
{
    const auto key = Key(relative);
    const std::scoped_lock lock(m_mutex);
    return m_deleted.contains(key);
}

bool TitleFileSystem::Exists(const std::filesystem::path& relative) const
{
    return !IsDeleted(relative) && (PathExists(m_overlayRoot / relative) || PathExists(m_titleRoot / relative));
}

std::optional<std::filesystem::path> TitleFileSystem::ReadPath(const std::filesystem::path& relative) const
{
    if (IsDeleted(relative))
    {
        return std::nullopt;
    }
    auto overlay = m_overlayRoot / relative;
    return PathExists(overlay) ? overlay : m_titleRoot / relative;
}

std::optional<std::filesystem::path> TitleFileSystem::WritePath(const std::filesystem::path& relative,
                                                                bool copyExisting)
{
    auto target = (m_overlayRoot / relative).lexically_normal();
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error && !std::filesystem::is_directory(target.parent_path()))
    {
        return std::nullopt;
    }
    if (copyExisting && !PathExists(target) && !IsDeleted(relative))
    {
        // Nothing at `source` just means a new file.
        const auto source = m_titleRoot / relative;
        std::error_code copyError;
        if (std::filesystem::is_directory(source, copyError))
        {
            std::filesystem::create_directories(target, copyError);
        }
        else if (std::filesystem::is_regular_file(source, copyError))
        {
            std::filesystem::copy_file(source, target, copyError);
            if (copyError)
            {
                return std::nullopt;
            }
        }
    }
    return target;
}

std::error_code TitleFileSystem::Delete(const std::filesystem::path& relative)
{
    if (!Exists(relative))
    {
        return std::make_error_code(std::errc::no_such_file_or_directory);
    }
    std::error_code error;
    const auto overlay = m_overlayRoot / relative;
    if (std::filesystem::is_regular_file(overlay, error))
    {
        std::filesystem::remove(overlay, error);
        if (error)
        {
            return error;
        }
    }
    if (PathExists(m_titleRoot / relative))
    {
        const std::scoped_lock lock(m_mutex);
        m_deleted.insert(Key(relative));
        SaveDeleted();
    }
    return {};
}

void TitleFileSystem::Undelete(const std::filesystem::path& relative)
{
    const auto key = Key(relative);
    const std::scoped_lock lock(m_mutex);
    if (m_deleted.erase(key) != 0)
    {
        SaveDeleted();
    }
}

bool TitleFileSystem::IsInternal(const std::filesystem::path& relative)
{
    return Key(relative) == kDeletedFile;
}

void TitleFileSystem::SaveDeleted() const
{
    std::ofstream stream(m_overlayRoot / kDeletedFile, std::ios::trunc);
    for (const auto& key : m_deleted)
    {
        const auto utf8 = std::filesystem::path(key).u8string();
        stream << std::string(utf8.begin(), utf8.end()) << '\n';
    }
}

void TitleFileSystem::SetActive(TitleFileSystem* fileSystem) noexcept
{
    ActiveFileSystem().store(fileSystem);
}

TitleFileSystem* TitleFileSystem::Active() noexcept
{
    return ActiveFileSystem().load();
}
} // namespace OpenEra::Kernel
