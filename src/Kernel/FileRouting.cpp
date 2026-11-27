#include "FileRouting.h"

#include <algorithm>
#include <bit>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace OpenEra::Kernel::FileRouting
{
namespace
{
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

struct TrackedHandle
{
    TitleFileSystem* Files = nullptr;
    std::filesystem::path Relative;
    bool DeleteOnClose = false;
};

struct HandleTable
{
    std::mutex Mutex;
    std::unordered_map<HANDLE, TrackedHandle> Handles;
};

HandleTable& Tracked()
{
    static HandleTable table;
    return table;
}
} // namespace

std::optional<TitlePath> InTitle(const wchar_t* name)
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

std::wstring FromAnsi(const char* text)
{
    if (text == nullptr)
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (length <= 1)
    {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), length);
    result.pop_back(); // Terminator.
    return result;
}

std::string ToAnsi(std::wstring_view text)
{
    const std::wstring terminated(text);
    const int length = WideCharToMultiByte(CP_ACP, 0, terminated.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
    {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, terminated.c_str(), -1, result.data(), length, nullptr, nullptr);
    result.pop_back(); // Terminator.
    return result;
}

Routed ForRead(const TitlePath& path)
{
    const auto read = path.Files.get().ReadPath(path.Relative);
    if (!read)
    {
        return {.HostPath = std::nullopt, .Error = ERROR_FILE_NOT_FOUND};
    }
    return {.HostPath = read->wstring(), .Error = ERROR_SUCCESS};
}

Routed ForWrite(const TitlePath& path, bool copyExisting)
{
    const auto write = path.Files.get().WritePath(path.Relative, copyExisting);
    if (!write)
    {
        return {.HostPath = std::nullopt, .Error = ERROR_PATH_NOT_FOUND};
    }
    return {.HostPath = write->wstring(), .Error = ERROR_SUCCESS};
}

DWORD ErrorFor(const std::error_code& error) noexcept
{
    if (!error)
    {
        return ERROR_SUCCESS;
    }
    if (error == std::errc::no_such_file_or_directory)
    {
        return ERROR_FILE_NOT_FOUND;
    }
    if (error == std::errc::directory_not_empty)
    {
        return ERROR_DIR_NOT_EMPTY;
    }
    if (error == std::errc::not_a_directory)
    {
        return ERROR_DIRECTORY;
    }
    return std::bit_cast<DWORD>(error.value());
}

DWORD Delete(const TitlePath& path)
{
    return ErrorFor(path.Files.get().Delete(path.Relative));
}

DWORD MakeDirectory(const TitlePath& path, LPSECURITY_ATTRIBUTES security)
{
    auto& files = path.Files.get();
    if (files.Exists(path.Relative))
    {
        return ERROR_ALREADY_EXISTS;
    }
    const auto parent = path.Relative.parent_path();
    if (!parent.empty() && !files.Exists(parent))
    {
        return ERROR_PATH_NOT_FOUND;
    }
    const auto write = files.WritePath(path.Relative, false);
    if (!write)
    {
        return ERROR_PATH_NOT_FOUND;
    }
    if (CreateDirectoryW(write->c_str(), security) == FALSE)
    {
        return GetLastError();
    }
    files.Undelete(path.Relative);
    return ERROR_SUCCESS;
}

DWORD RemoveDirectoryAt(const TitlePath& path)
{
    return ErrorFor(path.Files.get().RemoveEmptyDirectory(path.Relative));
}

DWORD Move(const wchar_t* from, const wchar_t* to, DWORD flags)
{
    const auto source = InTitle(from);
    const auto target = InTitle(to);
    const bool replace = (flags & MOVEFILE_REPLACE_EXISTING) != 0;

    std::wstring sourceHost = from == nullptr ? std::wstring() : std::wstring(from);
    bool copyFromFolder = false; // The folder's own file: copy it, never move it.
    if (source)
    {
        auto& files = source->Files.get();
        const auto read = files.ReadPath(source->Relative);
        if (!read || !files.Exists(source->Relative))
        {
            return ERROR_FILE_NOT_FOUND;
        }
        sourceHost = read->wstring();
        copyFromFolder = !files.InOverlay(source->Relative);
        std::error_code error;
        if (copyFromFolder && std::filesystem::is_directory(*read, error))
        {
            return ERROR_ACCESS_DENIED; // Moving the folder's own directories isn't supported.
        }
    }

    std::wstring targetHost = to == nullptr ? std::wstring() : std::wstring(to);
    if (target)
    {
        auto& files = target->Files.get();
        if (files.Exists(target->Relative) && !replace)
        {
            return ERROR_ALREADY_EXISTS;
        }
        const auto write = files.WritePath(target->Relative, false);
        if (!write)
        {
            return ERROR_PATH_NOT_FOUND;
        }
        targetHost = write->wstring();
    }

    const BOOL moved = copyFromFolder
                           ? CopyFileW(sourceHost.c_str(), targetHost.c_str(), replace ? FALSE : TRUE)
                           : MoveFileExW(sourceHost.c_str(), targetHost.c_str(), flags | MOVEFILE_COPY_ALLOWED);
    if (moved == FALSE)
    {
        return GetLastError();
    }
    if (source)
    {
        source->Files.get().Hide(source->Relative);
    }
    if (target)
    {
        target->Files.get().Undelete(target->Relative);
    }
    return ERROR_SUCCESS;
}

std::vector<WIN32_FIND_DATAW> Matches(const TitlePath& pattern, FINDEX_INFO_LEVELS level, FINDEX_SEARCH_OPS search,
                                      DWORD flags)
{
    const auto& files = pattern.Files.get();
    const std::filesystem::path directory = pattern.Relative.parent_path();
    const std::filesystem::path spec = pattern.Relative.filename();
    std::vector<WIN32_FIND_DATAW> entries;
    if (!directory.empty() && !files.Exists(directory))
    {
        return entries; // The directory itself was deleted, or never existed.
    }
    std::unordered_map<std::wstring, size_t> byName;
    AddMatches(files.OverlayRoot() / directory / spec, level, search, flags, entries, byName,
               [&](const std::wstring& name) { return TitleFileSystem::IsInternal(directory / name); });
    AddMatches(files.TitleRoot() / directory / spec, level, search, flags, entries, byName,
               [&](const std::wstring& name)
               { return name != L"." && name != L".." && files.IsDeleted(directory / name); });
    return entries;
}

void TrackHandle(HANDLE handle, const TitlePath& path, bool deleteOnClose)
{
    auto& table = Tracked();
    const std::scoped_lock lock(table.Mutex);
    table.Handles.insert_or_assign(
        handle, TrackedHandle{.Files = &path.Files.get(), .Relative = path.Relative, .DeleteOnClose = deleteOnClose});
}

DWORD CanDeleteThrough(HANDLE handle)
{
    auto& table = Tracked();
    const std::scoped_lock lock(table.Mutex);
    const auto it = table.Handles.find(handle);
    if (it == table.Handles.end())
    {
        return ERROR_SUCCESS;
    }
    const auto& tracked = it->second;
    const auto read = tracked.Files->ReadPath(tracked.Relative);
    std::error_code error;
    if (read && std::filesystem::is_directory(*read, error) && !tracked.Files->IsEmptyDirectory(tracked.Relative))
    {
        return ERROR_DIR_NOT_EMPTY;
    }
    return ERROR_SUCCESS;
}

void HandleDeleted(HANDLE handle)
{
    auto& table = Tracked();
    const std::scoped_lock lock(table.Mutex);
    const auto it = table.Handles.find(handle);
    if (it != table.Handles.end())
    {
        it->second.Files->Hide(it->second.Relative);
    }
}

void HandleClosed(HANDLE handle)
{
    auto& table = Tracked();
    const std::scoped_lock lock(table.Mutex);
    const auto it = table.Handles.find(handle);
    if (it == table.Handles.end())
    {
        return;
    }
    if (it->second.DeleteOnClose)
    {
        it->second.Files->Hide(it->second.Relative);
    }
    table.Handles.erase(it);
}
} // namespace OpenEra::Kernel::FileRouting
