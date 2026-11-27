#include "FileShims.h"

#include <bit>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Windows.h>

#include "Kernel/FileRouting.h"
#include "Kernel/ShimGuard.h"
#include "Kernel/ShimModule.h"

namespace OpenEra::Kernel
{
namespace
{
using FileRouting::InTitle;
using FileRouting::TitlePath;

constexpr DWORD kWriteAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA |
                               FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER;

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

// A CreateFile-style open of `name`: through the layer for title paths, unchanged otherwise.
template <typename Open>
HANDLE OpenRouted(LPCWSTR name, DWORD access, DWORD disposition, DWORD flags, Open open)
{
    const auto path = InTitle(name);
    if (!path)
    {
        return open(name);
    }
    const bool writes = WantsWrite(*path, access, disposition, flags);
    // CREATE_ALWAYS replaces the contents, so there's nothing to copy.
    const auto routed =
        writes ? FileRouting::ForWrite(*path, disposition != CREATE_ALWAYS) : FileRouting::ForRead(*path);
    if (!routed.HostPath)
    {
        SetLastError(routed.Error);
        return INVALID_HANDLE_VALUE;
    }
    auto* const file = open(routed.HostPath->c_str());
    if (file != INVALID_HANDLE_VALUE && writes)
    {
        path->Files.get().Undelete(path->Relative);
        const bool deleteOnClose = (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0;
        if ((access & DELETE) != 0 || deleteOnClose)
        {
            FileRouting::TrackHandle(file, *path, deleteOnClose);
        }
    }
    return file;
}

// Runs `routed` for title paths (returning a Win32 error) and `host` otherwise.
template <typename Routed, typename Host>
BOOL RunRouted(LPCWSTR name, Routed routed, Host host)
{
    const auto path = InTitle(name);
    if (!path)
    {
        return host();
    }
    const DWORD error = routed(*path);
    SetLastError(error);
    return error == ERROR_SUCCESS ? TRUE : FALSE;
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

HANDLE WINAPI CreateFileWShim(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                              DWORD disposition, DWORD flags, HANDLE templateFile) noexcept
{
    return GuardShim<HANDLE>(
        "CreateFileW", INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE,
        [=]
        {
            return OpenRouted(name, access, disposition, flags, [=](LPCWSTR path) noexcept
                              { return CreateFileW(path, access, share, security, disposition, flags, templateFile); });
        });
}

HANDLE WINAPI CreateFileAShim(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition,
                              DWORD flags, HANDLE templateFile) noexcept
{
    return GuardShim<HANDLE>(
        "CreateFileA", INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE,
        [=]() -> HANDLE
        {
            if (name == nullptr || TitleFileSystem::Active() == nullptr)
            {
                return CreateFileA(name, access, share, security, disposition, flags, templateFile);
            }
            const std::wstring wide = FileRouting::FromAnsi(name);
            return CreateFileWShim(wide.c_str(), access, share, security, disposition, flags, templateFile);
        });
}

HANDLE WINAPI CreateFile2Shim(LPCWSTR name, DWORD access, DWORD share, DWORD disposition,
                              LPCREATEFILE2_EXTENDED_PARAMETERS parameters) noexcept
{
    return GuardShim<HANDLE>("CreateFile2", INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE,
                             [=]
                             {
                                 const DWORD flags = parameters == nullptr ? 0 : parameters->dwFileFlags;
                                 return OpenRouted(
                                     name, access, disposition, flags, [=](LPCWSTR path) noexcept
                                     { return CreateFile2(path, access, share, disposition, parameters); });
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
                                const auto read = FileRouting::ForRead(*path);
                                if (!read.HostPath)
                                {
                                    SetLastError(read.Error);
                                    return INVALID_FILE_ATTRIBUTES;
                                }
                                return GetFileAttributesW(read.HostPath->c_str());
                            });
}

DWORD WINAPI GetFileAttributesAShim(LPCSTR name) noexcept
{
    return GuardShim<DWORD>("GetFileAttributesA", INVALID_FILE_ATTRIBUTES, INVALID_FILE_ATTRIBUTES,
                            [=]() -> DWORD
                            {
                                if (name == nullptr || TitleFileSystem::Active() == nullptr)
                                {
                                    return GetFileAttributesA(name);
                                }
                                return GetFileAttributesWShim(FileRouting::FromAnsi(name).c_str());
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
                               const auto read = FileRouting::ForRead(*path);
                               if (!read.HostPath)
                               {
                                   SetLastError(read.Error);
                                   return FALSE;
                               }
                               return GetFileAttributesExW(read.HostPath->c_str(), level, information);
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
                                 auto state = std::make_unique<FindState>();
                                 state->Entries = FileRouting::Matches(*path, level, search, flags);
                                 if (state->Entries.empty())
                                 {
                                     const auto directory = path->Relative.parent_path();
                                     const bool directoryVisible =
                                         directory.empty() || path->Files.get().Exists(directory);
                                     SetLastError(directoryVisible ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
                                     return INVALID_HANDLE_VALUE;
                                 }
                                 // Both information levels fill a WIN32_FIND_DATAW.
                                 *static_cast<WIN32_FIND_DATAW*>(data) = state->Entries.front();
                                 state->Next = 1;
                                 return Handles().Add(std::move(state));
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
                           [=]
                           {
                               return RunRouted(
                                   name,
                                   [=](const TitlePath& path) { return FileRouting::MakeDirectory(path, security); },
                                   [=]() noexcept { return CreateDirectoryW(name, security); });
                           });
}

BOOL WINAPI CreateDirectoryAShim(LPCSTR name, LPSECURITY_ATTRIBUTES security) noexcept
{
    return GuardShim<BOOL>("CreateDirectoryA", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               if (name == nullptr || TitleFileSystem::Active() == nullptr)
                               {
                                   return CreateDirectoryA(name, security);
                               }
                               return CreateDirectoryWShim(FileRouting::FromAnsi(name).c_str(), security);
                           });
}

BOOL WINAPI RemoveDirectoryWShim(LPCWSTR name) noexcept
{
    return GuardShim<BOOL>("RemoveDirectoryW", FALSE, FALSE,
                           [=]
                           {
                               return RunRouted(
                                   name, [](const TitlePath& path) { return FileRouting::RemoveDirectoryAt(path); },
                                   [=]() noexcept { return RemoveDirectoryW(name); });
                           });
}

BOOL WINAPI RemoveDirectoryAShim(LPCSTR name) noexcept
{
    return GuardShim<BOOL>("RemoveDirectoryA", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               if (name == nullptr || TitleFileSystem::Active() == nullptr)
                               {
                                   return RemoveDirectoryA(name);
                               }
                               return RemoveDirectoryWShim(FileRouting::FromAnsi(name).c_str());
                           });
}

BOOL WINAPI DeleteFileWShim(LPCWSTR name) noexcept
{
    return GuardShim<BOOL>("DeleteFileW", FALSE, FALSE,
                           [=]
                           {
                               return RunRouted(
                                   name, [](const TitlePath& path) { return FileRouting::Delete(path); },
                                   [=]() noexcept { return DeleteFileW(name); });
                           });
}

BOOL WINAPI DeleteFileAShim(LPCSTR name) noexcept
{
    return GuardShim<BOOL>("DeleteFileA", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               if (name == nullptr || TitleFileSystem::Active() == nullptr)
                               {
                                   return DeleteFileA(name);
                               }
                               return DeleteFileWShim(FileRouting::FromAnsi(name).c_str());
                           });
}

BOOL WINAPI MoveFileExWShim(LPCWSTR from, LPCWSTR to, DWORD flags) noexcept
{
    return GuardShim<BOOL>("MoveFileExW", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               if (!InTitle(from) && !InTitle(to))
                               {
                                   return MoveFileExW(from, to, flags);
                               }
                               const DWORD error = FileRouting::Move(from, to, flags);
                               SetLastError(error);
                               return error == ERROR_SUCCESS ? TRUE : FALSE;
                           });
}

// Host paths for a copy where either side is in the title folder: the source
// read through the layer, the target written to the overlay. Nullopt passes through.
struct CopyRoute
{
    std::wstring Source;
    std::wstring Target;
    std::optional<TitlePath> TargetTitle;
    DWORD Error = ERROR_SUCCESS;
};

std::optional<CopyRoute> RouteCopy(LPCWSTR from, LPCWSTR to, bool failIfExists)
{
    const auto source = InTitle(from);
    const auto target = InTitle(to);
    if (!source && !target)
    {
        return std::nullopt;
    }
    CopyRoute route{.Source = from == nullptr ? L"" : from,
                    .Target = to == nullptr ? L"" : to,
                    .TargetTitle = std::nullopt,
                    .Error = ERROR_SUCCESS};
    if (source)
    {
        const auto read = FileRouting::ForRead(*source);
        if (!read.HostPath || !source->Files.get().Exists(source->Relative))
        {
            route.Error = ERROR_FILE_NOT_FOUND;
            return route;
        }
        route.Source = *read.HostPath;
    }
    if (target)
    {
        if (failIfExists && target->Files.get().Exists(target->Relative))
        {
            route.Error = ERROR_FILE_EXISTS;
            return route;
        }
        const auto write = FileRouting::ForWrite(*target, false);
        if (!write.HostPath)
        {
            route.Error = write.Error;
            return route;
        }
        route.Target = *write.HostPath;
        route.TargetTitle = target;
    }
    return route;
}

void CopyFinished(const CopyRoute& route, bool copied)
{
    if (copied && route.TargetTitle)
    {
        route.TargetTitle->Files.get().Undelete(route.TargetTitle->Relative);
    }
}

BOOL WINAPI CopyFileExWShim(LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE progress, LPVOID data, LPBOOL cancel,
                            DWORD flags) noexcept
{
    return GuardShim<BOOL>("CopyFileExW", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               const auto route = RouteCopy(from, to, (flags & COPY_FILE_FAIL_IF_EXISTS) != 0);
                               if (!route)
                               {
                                   return CopyFileExW(from, to, progress, data, cancel, flags);
                               }
                               if (route->Error != ERROR_SUCCESS)
                               {
                                   SetLastError(route->Error);
                                   return FALSE;
                               }
                               const BOOL copied = CopyFileExW(route->Source.c_str(), route->Target.c_str(), progress,
                                                               data, cancel, flags);
                               CopyFinished(*route, copied != FALSE);
                               return copied;
                           });
}

BOOL WINAPI CopyFileWShim(LPCWSTR from, LPCWSTR to, BOOL failIfExists) noexcept
{
    return CopyFileExWShim(from, to, nullptr, nullptr, nullptr, failIfExists != FALSE ? COPY_FILE_FAIL_IF_EXISTS : 0);
}

BOOL WINAPI CopyFileAShim(LPCSTR from, LPCSTR to, BOOL failIfExists) noexcept
{
    return GuardShim<BOOL>("CopyFileA", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               if (from == nullptr || to == nullptr || TitleFileSystem::Active() == nullptr)
                               {
                                   return CopyFileA(from, to, failIfExists);
                               }
                               return CopyFileWShim(FileRouting::FromAnsi(from).c_str(),
                                                    FileRouting::FromAnsi(to).c_str(), failIfExists);
                           });
}

HRESULT WINAPI CopyFile2Shim(PCWSTR from, PCWSTR to, COPYFILE2_EXTENDED_PARAMETERS* parameters) noexcept
{
    return std::bit_cast<HRESULT>(
        GuardHResult("CopyFile2",
                     [=]() -> HResult
                     {
                         const DWORD flags = parameters == nullptr ? 0 : parameters->dwCopyFlags;
                         const auto route = RouteCopy(from, to, (flags & COPY_FILE_FAIL_IF_EXISTS) != 0);
                         if (!route)
                         {
                             return std::bit_cast<HResult>(CopyFile2(from, to, parameters));
                         }
                         if (route->Error != ERROR_SUCCESS)
                         {
                             return std::bit_cast<HResult>(HRESULT_FROM_WIN32(route->Error));
                         }
                         const HRESULT copied = CopyFile2(route->Source.c_str(), route->Target.c_str(), parameters);
                         CopyFinished(*route, SUCCEEDED(copied));
                         return std::bit_cast<HResult>(copied);
                     }));
}

BOOL WINAPI MoveFileWShim(LPCWSTR from, LPCWSTR to) noexcept
{
    return MoveFileExWShim(from, to, MOVEFILE_COPY_ALLOWED);
}

BOOL WINAPI MoveFileExAShim(LPCSTR from, LPCSTR to, DWORD flags) noexcept
{
    return GuardShim<BOOL>("MoveFileExA", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               if (from == nullptr || to == nullptr || TitleFileSystem::Active() == nullptr)
                               {
                                   return MoveFileExA(from, to, flags);
                               }
                               return MoveFileExWShim(FileRouting::FromAnsi(from).c_str(),
                                                      FileRouting::FromAnsi(to).c_str(), flags);
                           });
}

BOOL WINAPI MoveFileAShim(LPCSTR from, LPCSTR to) noexcept
{
    return MoveFileExAShim(from, to, MOVEFILE_COPY_ALLOWED);
}

// A delete through a handle OpenEra opened in the overlay also hides the folder's file.
BOOL WINAPI SetFileInformationByHandleShim(HANDLE file, FILE_INFO_BY_HANDLE_CLASS type, LPVOID information,
                                           DWORD size) noexcept
{
    return GuardShim<BOOL>("SetFileInformationByHandle", FALSE, FALSE,
                           [=]() -> BOOL
                           {
                               if (information == nullptr)
                               {
                                   return SetFileInformationByHandle(file, type, information, size);
                               }
                               bool deletes = false;
                               if (type == FileDispositionInfo && size >= sizeof(FILE_DISPOSITION_INFO))
                               {
                                   deletes =
                                       static_cast<const FILE_DISPOSITION_INFO*>(information)->DeleteFile != FALSE;
                               }
                               else if (type == FileDispositionInfoEx && size >= sizeof(FILE_DISPOSITION_INFO_EX))
                               {
                                   deletes = (static_cast<const FILE_DISPOSITION_INFO_EX*>(information)->Flags &
                                              FILE_DISPOSITION_FLAG_DELETE) != 0;
                               }
                               if (deletes)
                               {
                                   if (const DWORD error = FileRouting::CanDeleteThrough(file); error != ERROR_SUCCESS)
                                   {
                                       SetLastError(error);
                                       return FALSE;
                                   }
                               }
                               const BOOL set = SetFileInformationByHandle(file, type, information, size);
                               if (set != FALSE && deletes)
                               {
                                   FileRouting::HandleDeleted(file);
                               }
                               return set;
                           });
}

BOOL WINAPI CloseHandleShim(HANDLE handle) noexcept
{
    return GuardShim<BOOL>("CloseHandle", FALSE, FALSE,
                           [=]
                           {
                               FileRouting::HandleClosed(handle);
                               return CloseHandle(handle);
                           });
}
} // namespace

bool AddFileExports(ShimModule& kernel32)
{
    return kernel32.Export("CreateFileW", ToShimProc(&CreateFileWShim)) &&
           kernel32.Export("CreateFileA", ToShimProc(&CreateFileAShim)) &&
           kernel32.Export("CreateFile2", ToShimProc(&CreateFile2Shim)) &&
           kernel32.Export("GetFileAttributesW", ToShimProc(&GetFileAttributesWShim)) &&
           kernel32.Export("GetFileAttributesA", ToShimProc(&GetFileAttributesAShim)) &&
           kernel32.Export("GetFileAttributesExW", ToShimProc(&GetFileAttributesExWShim)) &&
           kernel32.Export("FindFirstFileW", ToShimProc(&FindFirstFileWShim)) &&
           kernel32.Export("FindFirstFileExW", ToShimProc(&FindFirstFileExWShim)) &&
           kernel32.Export("FindNextFileW", ToShimProc(&FindNextFileWShim)) &&
           kernel32.Export("FindClose", ToShimProc(&FindCloseShim)) &&
           kernel32.Export("CreateDirectoryW", ToShimProc(&CreateDirectoryWShim)) &&
           kernel32.Export("CreateDirectoryA", ToShimProc(&CreateDirectoryAShim)) &&
           kernel32.Export("RemoveDirectoryW", ToShimProc(&RemoveDirectoryWShim)) &&
           kernel32.Export("RemoveDirectoryA", ToShimProc(&RemoveDirectoryAShim)) &&
           kernel32.Export("DeleteFileW", ToShimProc(&DeleteFileWShim)) &&
           kernel32.Export("DeleteFileA", ToShimProc(&DeleteFileAShim)) &&
           kernel32.Export("MoveFileExW", ToShimProc(&MoveFileExWShim)) &&
           kernel32.Export("MoveFileW", ToShimProc(&MoveFileWShim)) &&
           kernel32.Export("MoveFileExA", ToShimProc(&MoveFileExAShim)) &&
           kernel32.Export("MoveFileA", ToShimProc(&MoveFileAShim)) &&
           kernel32.Export("CopyFileExW", ToShimProc(&CopyFileExWShim)) &&
           kernel32.Export("CopyFileW", ToShimProc(&CopyFileWShim)) &&
           kernel32.Export("CopyFileA", ToShimProc(&CopyFileAShim)) &&
           kernel32.Export("CopyFile2", ToShimProc(&CopyFile2Shim)) &&
           kernel32.Export("SetFileInformationByHandle", ToShimProc(&SetFileInformationByHandleShim)) &&
           kernel32.Export("CloseHandle", ToShimProc(&CloseHandleShim));
}
} // namespace OpenEra::Kernel
