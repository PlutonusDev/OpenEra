#include "FileShims.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <Windows.h>

#include "Kernel/ShimGuard.h"
#include "Kernel/ShimModule.h"
#include "Kernel/TitleFileSystem.h"

namespace OpenEra::Kernel
{
namespace
{
constexpr DWORD kWriteAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA |
                               FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;

struct TitlePath
{
    std::reference_wrapper<TitleFileSystem> Files;
    std::filesystem::path Relative;
};

// The active title file system and `name` relative to its folder, if it's inside.
std::optional<TitlePath> InTitle(LPCWSTR name)
{
    TitleFileSystem* const files = TitleFileSystem::Active();
    if (files == nullptr || name == nullptr || *name == L'\0')
    {
        return std::nullopt;
    }
    auto relative = files->TitleRelative(std::filesystem::path(name));
    if (!relative)
    {
        return std::nullopt;
    }
    return TitlePath{.Files = *files, .Relative = std::move(*relative)};
}

bool WantsWrite(const TitlePath& path, DWORD access, DWORD disposition, DWORD flags)
{
    if ((access & kWriteAccess) != 0 || (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0)
    {
        return true;
    }
    switch (disposition)
    {
    case CREATE_NEW:
    case CREATE_ALWAYS:
    case TRUNCATE_EXISTING:
        return true;
    case OPEN_ALWAYS:
        return !path.Files.get().Exists(path.Relative);
    default:
        return false;
    }
}

// FindFirstFile handles OpenEra creates for merged listings.
struct FindState
{
    std::vector<WIN32_FIND_DATAW> Entries;
    size_t Next = 0;
};

class FindHandles
{
public:
    [[nodiscard]] HANDLE Add(std::unique_ptr<FindState> state)
    {
        const std::scoped_lock lock(m_mutex);
        auto* const handle = std::bit_cast<HANDLE>(std::bit_cast<uintptr_t>(state.get()));
        m_states.emplace(handle, std::move(state));
        return handle;
    }

    // Copies the next entry; nullopt if `handle` isn't one of ours, false at the end.
    [[nodiscard]] std::optional<bool> Next(HANDLE handle, WIN32_FIND_DATAW& data)
    {
        const std::scoped_lock lock(m_mutex);
        const auto it = m_states.find(handle);
        if (it == m_states.end())
        {
            return std::nullopt;
        }
        auto& state = *it->second;
        if (state.Next >= state.Entries.size())
        {
            return false;
        }
        data = state.Entries.at(state.Next++);
        return true;
    }

    [[nodiscard]] bool Remove(HANDLE handle)
    {
        const std::scoped_lock lock(m_mutex);
        return m_states.erase(handle) != 0;
    }

private:
    std::mutex m_mutex;
    std::unordered_map<HANDLE, std::unique_ptr<FindState>> m_states;
};

FindHandles& Handles()
{
    static FindHandles handles;
    return handles;
}

constexpr wchar_t AsciiLower(wchar_t value) noexcept
{
    constexpr std::wstring_view kUpper = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    constexpr std::wstring_view kLower = L"abcdefghijklmnopqrstuvwxyz";
    const auto index = kUpper.find(value);
    return index == std::wstring_view::npos ? value : kLower.at(index);
}

std::wstring Lowered(std::wstring text)
{
    std::ranges::transform(text, text.begin(), AsciiLower);
    return text;
}

// Appends the matches of `pattern` in one layer, skipping names already listed.
template <typename Skip>
void AddMatches(const std::filesystem::path& pattern, FINDEX_INFO_LEVELS level, FINDEX_SEARCH_OPS search, DWORD flags,
                std::vector<WIN32_FIND_DATAW>& entries, std::unordered_map<std::wstring, size_t>& byName, Skip skip)
{
    WIN32_FIND_DATAW data{};
    auto* const find = FindFirstFileExW(pattern.c_str(), level, &data, search, nullptr, flags);
    if (find == INVALID_HANDLE_VALUE)
    {
        return;
    }
    do
    {
        const auto& fileName = data.cFileName;
        const std::wstring name(std::begin(fileName), std::ranges::find(fileName, L'\0'));
        if (!skip(name) && byName.emplace(Lowered(name), entries.size()).second)
        {
            entries.push_back(data);
        }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
}

HANDLE FindInTitle(const TitlePath& path, FINDEX_INFO_LEVELS level, WIN32_FIND_DATAW& first, FINDEX_SEARCH_OPS search,
                   DWORD flags)
{
    const std::filesystem::path directory = path.Relative.parent_path();
    const std::filesystem::path spec = path.Relative.filename();
    auto state = std::make_unique<FindState>();
    std::unordered_map<std::wstring, size_t> byName;
    // Overlay entries win over the folder's; deleted and internal names are hidden.
    AddMatches(path.Files.get().OverlayRoot() / directory / spec, level, search, flags, state->Entries, byName,
               [&](const std::wstring& name) { return TitleFileSystem::IsInternal(directory / name); });
    AddMatches(path.Files.get().TitleRoot() / directory / spec, level, search, flags, state->Entries, byName,
               [&](const std::wstring& name)
               { return name != L"." && name != L".." && path.Files.get().IsDeleted(directory / name); });
    if (state->Entries.empty())
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    first = state->Entries.front();
    state->Next = 1;
    return Handles().Add(std::move(state));
}

HANDLE WINAPI CreateFileWShim(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                              DWORD disposition, DWORD flags, HANDLE templateFile) noexcept
{
    return GuardShim<HANDLE>(
        "CreateFileW", INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE,
        [=]() -> HANDLE
        {
            const auto path = InTitle(name);
            if (!path)
            {
                return CreateFileW(name, access, share, security, disposition, flags, templateFile);
            }
            if (!WantsWrite(*path, access, disposition, flags))
            {
                const auto read = path->Files.get().ReadPath(path->Relative);
                if (!read)
                {
                    SetLastError(ERROR_FILE_NOT_FOUND);
                    return INVALID_HANDLE_VALUE;
                }
                return CreateFileW(read->c_str(), access, share, security, disposition, flags, templateFile);
            }
            // CREATE_ALWAYS replaces the contents, so there's nothing to copy.
            const auto write = path->Files.get().WritePath(path->Relative, disposition != CREATE_ALWAYS);
            if (!write)
            {
                SetLastError(ERROR_PATH_NOT_FOUND);
                return INVALID_HANDLE_VALUE;
            }
            auto* const file = CreateFileW(write->c_str(), access, share, security, disposition, flags, templateFile);
            if (file != INVALID_HANDLE_VALUE)
            {
                path->Files.get().Undelete(path->Relative);
            }
            return file;
        });
}

DWORD WINAPI GetFileAttributesWShim(LPCWSTR name) noexcept
{
    return GuardShim<DWORD>("GetFileAttributesW", INVALID_FILE_ATTRIBUTES, INVALID_FILE_ATTRIBUTES,
                            [=]() -> DWORD
                            {
                                const auto path = InTitle(name);
                                if (!path)
                                {
                                    return GetFileAttributesW(name);
                                }
                                const auto read = path->Files.get().ReadPath(path->Relative);
                                if (!read)
                                {
                                    SetLastError(ERROR_FILE_NOT_FOUND);
                                    return INVALID_FILE_ATTRIBUTES;
                                }
                                return GetFileAttributesW(read->c_str());
                            });
}

BOOL WINAPI GetFileAttributesExWShim(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID information) noexcept
{
    return GuardShim<BOOL>("GetFileAttributesExW", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               const auto path = InTitle(name);
                               if (!path)
                               {
                                   return GetFileAttributesExW(name, level, information);
                               }
                               const auto read = path->Files.get().ReadPath(path->Relative);
                               if (!read)
                               {
                                   SetLastError(ERROR_FILE_NOT_FOUND);
                                   return FALSE;
                               }
                               return GetFileAttributesExW(read->c_str(), level, information);
                           });
}

HANDLE WINAPI FindFirstFileExWShim(LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search,
                                   LPVOID filter, DWORD flags) noexcept
{
    return GuardShim<HANDLE>("FindFirstFileExW", INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE,
                             [=]() -> HANDLE
                             {
                                 const auto path = InTitle(name);
                                 if (!path || data == nullptr || filter != nullptr)
                                 {
                                     return FindFirstFileExW(name, level, data, search, filter, flags);
                                 }
                                 // Both information levels fill a WIN32_FIND_DATAW.
                                 return FindInTitle(*path, level, *static_cast<WIN32_FIND_DATAW*>(data), search, flags);
                             });
}

HANDLE WINAPI FindFirstFileWShim(LPCWSTR name, LPWIN32_FIND_DATAW data) noexcept
{
    return FindFirstFileExWShim(name, FindExInfoStandard, data, FindExSearchNameMatch, nullptr, 0);
}

BOOL WINAPI FindNextFileWShim(HANDLE find, LPWIN32_FIND_DATAW data) noexcept
{
    return GuardShim<BOOL>("FindNextFileW", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               if (data == nullptr)
                               {
                                   return FindNextFileW(find, data);
                               }
                               const auto next = Handles().Next(find, *data);
                               if (!next)
                               {
                                   return FindNextFileW(find, data);
                               }
                               if (!*next)
                               {
                                   SetLastError(ERROR_NO_MORE_FILES);
                                   return FALSE;
                               }
                               return TRUE;
                           });
}

BOOL WINAPI FindCloseShim(HANDLE find) noexcept
{
    return GuardShim<BOOL>("FindClose", FALSE, FALSE,
                           [=]() -> BOOL { return Handles().Remove(find) ? TRUE : FindClose(find); });
}

BOOL WINAPI CreateDirectoryWShim(LPCWSTR name, LPSECURITY_ATTRIBUTES security) noexcept
{
    return GuardShim<BOOL>("CreateDirectoryW", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               const auto path = InTitle(name);
                               if (!path)
                               {
                                   return CreateDirectoryW(name, security);
                               }
                               if (path->Files.get().Exists(path->Relative))
                               {
                                   SetLastError(ERROR_ALREADY_EXISTS);
                                   return FALSE;
                               }
                               const auto parent = path->Relative.parent_path();
                               if (!parent.empty() && !path->Files.get().Exists(parent))
                               {
                                   SetLastError(ERROR_PATH_NOT_FOUND);
                                   return FALSE;
                               }
                               const auto write = path->Files.get().WritePath(path->Relative, false);
                               if (!write)
                               {
                                   SetLastError(ERROR_PATH_NOT_FOUND);
                                   return FALSE;
                               }
                               const BOOL created = CreateDirectoryW(write->c_str(), security);
                               if (created != FALSE)
                               {
                                   path->Files.get().Undelete(path->Relative);
                               }
                               return created;
                           });
}

BOOL WINAPI DeleteFileWShim(LPCWSTR name) noexcept
{
    return GuardShim<BOOL>("DeleteFileW", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               const auto path = InTitle(name);
                               if (!path)
                               {
                                   return DeleteFileW(name);
                               }
                               const auto error = path->Files.get().Delete(path->Relative);
                               if (error)
                               {
                                   SetLastError(error == std::errc::no_such_file_or_directory
                                                    ? ERROR_FILE_NOT_FOUND
                                                    : std::bit_cast<DWORD>(error.value()));
                                   return FALSE;
                               }
                               return TRUE;
                           });
}
} // namespace

bool AddFileExports(ShimModule& kernel32)
{
    return kernel32.Export("CreateFileW", ToShimProc(&CreateFileWShim)) &&
           kernel32.Export("GetFileAttributesW", ToShimProc(&GetFileAttributesWShim)) &&
           kernel32.Export("GetFileAttributesExW", ToShimProc(&GetFileAttributesExWShim)) &&
           kernel32.Export("FindFirstFileW", ToShimProc(&FindFirstFileWShim)) &&
           kernel32.Export("FindFirstFileExW", ToShimProc(&FindFirstFileExWShim)) &&
           kernel32.Export("FindNextFileW", ToShimProc(&FindNextFileWShim)) &&
           kernel32.Export("FindClose", ToShimProc(&FindCloseShim)) &&
           kernel32.Export("CreateDirectoryW", ToShimProc(&CreateDirectoryWShim)) &&
           kernel32.Export("DeleteFileW", ToShimProc(&DeleteFileWShim));
}
} // namespace OpenEra::Kernel
