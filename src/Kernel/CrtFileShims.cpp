#include "CrtFileShims.h"

#include <array>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Windows.h>
#include <io.h>
#include <sys/stat.h>

#include "Kernel/FileRouting.h"
#include "Kernel/ProcessShims.h"
#include "Kernel/ShimGuard.h"
#include "Kernel/ShimModule.h"

namespace OpenEra::Kernel
{
namespace
{
using FileRouting::InTitle;
using FileRouting::TitlePath;

// std::ios_base::openmode bits, as the MSVC STL defines them.
constexpr int kStreamIn = 0x01;
constexpr int kStreamOut = 0x02;
constexpr int kStreamAppend = 0x08;
constexpr int kStreamTruncate = 0x10;
static_assert(std::ios_base::in == kStreamIn && std::ios_base::out == kStreamOut &&
              std::ios_base::app == kStreamAppend && std::ios_base::trunc == kStreamTruncate);

constexpr uint64_t kUnixEpochFileTime = 116'444'736'000'000'000ULL; // 1970-01-01 in 100 ns units since 1601.
constexpr uint64_t kFileTimePerSecond = 10'000'000;

// The host's own C runtime DLLs, from System32 (the ones title imports pass through to).
HMODULE HostModule(const wchar_t* name) noexcept
{
    return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

template <typename Function>
Function* HostFunction(HMODULE module, const char* name) noexcept
{
    auto* const proc = module == nullptr ? nullptr : GetProcAddress(module, name);
    return std::bit_cast<Function*>(std::bit_cast<uintptr_t>(proc));
}

template <typename Function>
Function* Ucrt(const char* name) noexcept
{
    static const HMODULE kUcrt = HostModule(L"ucrtbase.dll");
    return HostFunction<Function>(kUcrt, name);
}

template <typename Function>
Function* Msvcp(const char* name) noexcept
{
    static const HMODULE kMsvcp = HostModule(L"msvcp140.dll");
    return HostFunction<Function>(kMsvcp, name);
}

// Calls a host function by name; a missing export fails with ENOSYS instead of crashing.
template <typename Result, typename... Args>
Result CallUcrt(const char* name, Result failure, Args... args) noexcept;

template <typename Result, typename... Args>
Result CallMsvcp(const char* name, Result failure, Args... args) noexcept;

void SetErrno(int value) noexcept
{
    if (auto* const location = Ucrt<int*() noexcept>("_errno"))
    {
        *location() = value;
    }
}

// errno as the host C runtime (the one the title uses) has it.
int HostErrno() noexcept
{
    auto* const location = Ucrt<int*() noexcept>("_errno");
    return location == nullptr ? EINVAL : *location();
}

template <typename Result, typename... Args>
Result CallUcrt(const char* name, Result failure, Args... args) noexcept
{
    auto* const function = Ucrt<Result(Args...) noexcept>(name);
    if (function == nullptr)
    {
        SetErrno(ENOSYS);
        return failure;
    }
    return function(args...);
}

template <typename Result, typename... Args>
Result CallMsvcp(const char* name, Result failure, Args... args) noexcept
{
    auto* const function = Msvcp<Result(Args...) noexcept>(name);
    if (function == nullptr)
    {
        SetErrno(ENOSYS);
        return failure;
    }
    return function(args...);
}

constexpr int ErrnoFor(DWORD error) noexcept
{
    switch (error)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ENOENT;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return EEXIST;
    case ERROR_DIR_NOT_EMPTY:
        return ENOTEMPTY;
    case ERROR_DIRECTORY:
        return ENOTDIR;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return EACCES;
    default:
        return EINVAL;
    }
}

// 0 on success; otherwise errno is set and -1 returned (the C runtime's convention).
int CrtResult(DWORD error) noexcept
{
    if (error == ERROR_SUCCESS)
    {
        return 0;
    }
    SetErrno(ErrnoFor(error));
    return -1;
}

// For a title path, `open` on the resolved host path; nullopt to pass `path` through.
template <typename Result, typename Open>
std::optional<Result> OpenInTitle(const wchar_t* path, bool writes, bool keepContents, Result failure, Open open)
{
    const auto title = InTitle(path);
    if (!title)
    {
        return std::nullopt;
    }
    const auto routed = writes ? FileRouting::ForWrite(*title, keepContents) : FileRouting::ForRead(*title);
    if (!routed.HostPath)
    {
        SetErrno(ErrnoFor(routed.Error));
        return failure;
    }
    Result result = open(routed.HostPath->c_str());
    if (writes && result != failure)
    {
        title->Files.get().Undelete(title->Relative);
    }
    return result;
}

std::wstring Wide(const char* text)
{
    return FileRouting::FromAnsi(text);
}

bool Routes(const char* path)
{
    return path != nullptr && TitleFileSystem::Active() != nullptr && InTitle(Wide(path).c_str()).has_value();
}

// --- Opening files -------------------------------------------------------------------------------

FILE* OpenFile(const wchar_t* path, const wchar_t* mode, int share)
{
    const std::wstring_view modeText = mode == nullptr ? L"" : mode;
    const auto open = [=](const wchar_t* target) noexcept
    { return CallUcrt("_wfsopen", static_cast<FILE*>(nullptr), target, mode, share); };
    const auto routed = OpenInTitle<FILE*>(path, OpensForWrite(modeText), KeepsContents(modeText), nullptr, open);
    return routed ? *routed : open(path);
}

FILE* WideFopenShim(const wchar_t* path, const wchar_t* mode) noexcept
{
    return GuardShim<FILE*>("_wfopen", nullptr, nullptr, [=] { return OpenFile(path, mode, _SH_DENYNO); });
}

FILE* WideFsopenShim(const wchar_t* path, const wchar_t* mode, int share) noexcept
{
    return GuardShim<FILE*>("_wfsopen", nullptr, nullptr, [=] { return OpenFile(path, mode, share); });
}

FILE* FopenShim(const char* path, const char* mode) noexcept
{
    return GuardShim<FILE*>("fopen", nullptr, nullptr,
                            [=]() -> FILE*
                            {
                                if (!Routes(path))
                                {
                                    return CallUcrt("fopen", static_cast<FILE*>(nullptr), path, mode);
                                }
                                return OpenFile(Wide(path).c_str(), Wide(mode).c_str(), _SH_DENYNO);
                            });
}

FILE* FsopenShim(const char* path, const char* mode, int share) noexcept
{
    return GuardShim<FILE*>("_fsopen", nullptr, nullptr,
                            [=]() -> FILE*
                            {
                                if (!Routes(path))
                                {
                                    return CallUcrt("_fsopen", static_cast<FILE*>(nullptr), path, mode, share);
                                }
                                return OpenFile(Wide(path).c_str(), Wide(mode).c_str(), share);
                            });
}

errno_t WideFopenSShim(FILE** file, const wchar_t* path, const wchar_t* mode) noexcept
{
    return GuardShim<errno_t>("_wfopen_s", EINVAL, ENOMEM,
                              [=]() -> errno_t
                              {
                                  if (file == nullptr || !InTitle(path))
                                  {
                                      return CallUcrt("_wfopen_s", errno_t{EINVAL}, file, path, mode);
                                  }
                                  *file = OpenFile(path, mode, _SH_SECURE);
                                  return *file == nullptr ? HostErrno() : 0;
                              });
}

errno_t FopenSShim(FILE** file, const char* path, const char* mode) noexcept
{
    return GuardShim<errno_t>("fopen_s", EINVAL, ENOMEM,
                              [=]() -> errno_t
                              {
                                  if (file == nullptr || !Routes(path))
                                  {
                                      return CallUcrt("fopen_s", errno_t{EINVAL}, file, path, mode);
                                  }
                                  *file = OpenFile(Wide(path).c_str(), Wide(mode).c_str(), _SH_SECURE);
                                  return *file == nullptr ? HostErrno() : 0;
                              });
}

// --- Removing, renaming and directories ----------------------------------------------------------

template <typename Routed, typename Host>
int RunCrt(const wchar_t* path, Routed routed, Host host)
{
    const auto title = InTitle(path);
    return title ? CrtResult(routed(*title)) : host();
}

int WideRemoveShim(const wchar_t* path) noexcept
{
    return GuardShim<int>("_wremove", -1, -1,
                          [=]
                          {
                              return RunCrt(
                                  path, [](const TitlePath& title) { return FileRouting::Delete(title); },
                                  [=]() noexcept { return CallUcrt("_wremove", -1, path); });
                          });
}

int RemoveShim(const char* path) noexcept
{
    return GuardShim<int>("remove", -1, -1, [=]
                          { return Routes(path) ? WideRemoveShim(Wide(path).c_str()) : CallUcrt("remove", -1, path); });
}

int RenameIn(const wchar_t* from, const wchar_t* to)
{
    // rename never replaces an existing file, and reports that as EACCES.
    const DWORD error = FileRouting::Move(from, to, MOVEFILE_COPY_ALLOWED);
    if (error == ERROR_ALREADY_EXISTS)
    {
        SetErrno(EACCES);
        return -1;
    }
    return CrtResult(error);
}

int WideRenameShim(const wchar_t* from, const wchar_t* to) noexcept
{
    return GuardShim<int>(
        "_wrename", -1, -1,
        [=] { return InTitle(from) || InTitle(to) ? RenameIn(from, to) : CallUcrt("_wrename", -1, from, to); });
}

int RenameShim(const char* from, const char* to) noexcept
{
    return GuardShim<int>("rename", -1, -1,
                          [=]
                          {
                              return Routes(from) || Routes(to) ? RenameIn(Wide(from).c_str(), Wide(to).c_str())
                                                                : CallUcrt("rename", -1, from, to);
                          });
}

int WideMkdirShim(const wchar_t* path) noexcept
{
    return GuardShim<int>("_wmkdir", -1, -1,
                          [=]
                          {
                              return RunCrt(
                                  path,
                                  [](const TitlePath& title) { return FileRouting::MakeDirectory(title, nullptr); },
                                  [=]() noexcept { return CallUcrt("_wmkdir", -1, path); });
                          });
}

int MkdirShim(const char* path) noexcept
{
    return GuardShim<int>("_mkdir", -1, -1, [=]
                          { return Routes(path) ? WideMkdirShim(Wide(path).c_str()) : CallUcrt("_mkdir", -1, path); });
}

int WideRmdirShim(const wchar_t* path) noexcept
{
    return GuardShim<int>("_wrmdir", -1, -1,
                          [=]
                          {
                              return RunCrt(
                                  path, [](const TitlePath& title) { return FileRouting::RemoveDirectoryAt(title); },
                                  [=]() noexcept { return CallUcrt("_wrmdir", -1, path); });
                          });
}

int RmdirShim(const char* path) noexcept
{
    return GuardShim<int>("_rmdir", -1, -1, [=]
                          { return Routes(path) ? WideRmdirShim(Wide(path).c_str()) : CallUcrt("_rmdir", -1, path); });
}

int WideAccessShim(const wchar_t* path, int mode) noexcept
{
    return GuardShim<int>("_waccess", -1, -1,
                          [=]() -> int
                          {
                              const auto title = InTitle(path);
                              if (!title)
                              {
                                  return CallUcrt("_waccess", -1, path, mode);
                              }
                              const auto read = FileRouting::ForRead(*title);
                              if (!read.HostPath)
                              {
                                  return CrtResult(read.Error);
                              }
                              // Title files are always writable: writes go to the overlay.
                              return CallUcrt("_waccess", -1, read.HostPath->c_str(), 0);
                          });
}

int AccessShim(const char* path, int mode) noexcept
{
    return GuardShim<int>(
        "_access", -1, -1,
        [=] { return Routes(path) ? WideAccessShim(Wide(path).c_str(), mode) : CallUcrt("_access", -1, path, mode); });
}

// --- stat --------------------------------------------------------------------------------------

template <typename Stat>
int StatInTitle(const wchar_t* path, Stat* buffer, const char* wideName)
{
    const auto title = InTitle(path);
    if (!title)
    {
        return CallUcrt(wideName, -1, path, buffer);
    }
    const auto read = FileRouting::ForRead(*title);
    if (!read.HostPath)
    {
        return CrtResult(read.Error);
    }
    return CallUcrt(wideName, -1, read.HostPath->c_str(), buffer);
}

int WideStat64Shim(const wchar_t* path, struct _stat64* buffer) noexcept
{
    return GuardShim<int>("_wstat64", -1, -1, [=] { return StatInTitle(path, buffer, "_wstat64"); });
}

int Stat64Shim(const char* path, struct _stat64* buffer) noexcept
{
    return GuardShim<int>("_stat64", -1, -1,
                          [=]
                          {
                              return Routes(path) ? StatInTitle(Wide(path).c_str(), buffer, "_wstat64")
                                                  : CallUcrt("_stat64", -1, path, buffer);
                          });
}

int WideStat64i32Shim(const wchar_t* path, struct _stat64i32* buffer) noexcept
{
    return GuardShim<int>("_wstat64i32", -1, -1, [=] { return StatInTitle(path, buffer, "_wstat64i32"); });
}

int Stat64i32Shim(const char* path, struct _stat64i32* buffer) noexcept
{
    return GuardShim<int>("_stat64i32", -1, -1,
                          [=]
                          {
                              return Routes(path) ? StatInTitle(Wide(path).c_str(), buffer, "_wstat64i32")
                                                  : CallUcrt("_stat64i32", -1, path, buffer);
                          });
}

// --- _findfirst ----------------------------------------------------------------------------------

struct CrtFind
{
    std::vector<WIN32_FIND_DATAW> Entries;
    size_t Next = 0;
};

struct CrtFindTable
{
    std::mutex Mutex;
    std::unordered_map<intptr_t, std::unique_ptr<CrtFind>> Finds;
};

CrtFindTable& Finds()
{
    static CrtFindTable table;
    return table;
}

int64_t ToTimeT(const FILETIME& time) noexcept
{
    const uint64_t high = time.dwHighDateTime;
    const uint64_t value = (high << 32) | time.dwLowDateTime;
    if (value < kUnixEpochFileTime)
    {
        return -1;
    }
    return std::bit_cast<int64_t>((value - kUnixEpochFileTime) / kFileTimePerSecond);
}

// One merged entry in the C runtime's layout (Microsoft Learn: _findfirst).
template <typename Data>
void Fill(const WIN32_FIND_DATAW& from, Data& to)
{
    to.attrib = from.dwFileAttributes == FILE_ATTRIBUTE_NORMAL ? 0 : from.dwFileAttributes;
    to.time_create = ToTimeT(from.ftCreationTime);
    to.time_access = ToTimeT(from.ftLastAccessTime);
    to.time_write = ToTimeT(from.ftLastWriteTime);
    const uint64_t sizeHigh = from.nFileSizeHigh;
    const uint64_t size = (sizeHigh << 32) | from.nFileSizeLow;
    if constexpr (sizeof(to.size) == sizeof(uint64_t))
    {
        to.size = std::bit_cast<int64_t>(size);
    }
    else
    {
        to.size = std::bit_cast<std::array<unsigned long, 2>>(size).front();
    }
    const auto& source = from.cFileName;
    const std::wstring name(std::begin(source), std::ranges::find(source, L'\0'));
    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(to.name[0])>, wchar_t>)
    {
        std::ignore = CopyToBuffer(std::wstring_view(name), std::span(to.name));
    }
    else
    {
        std::ignore = CopyToBuffer(std::string_view(FileRouting::ToAnsi(name)), std::span(to.name));
    }
}

template <typename Data>
intptr_t FindFirstInTitle(const TitlePath& pattern, Data* data)
{
    auto find = std::make_unique<CrtFind>();
    find->Entries = FileRouting::Matches(pattern, FindExInfoStandard, FindExSearchNameMatch, 0);
    if (find->Entries.empty())
    {
        SetErrno(ENOENT);
        return -1;
    }
    Fill(find->Entries.front(), *data);
    find->Next = 1;
    auto& table = Finds();
    const std::scoped_lock lock(table.Mutex);
    const auto handle = std::bit_cast<intptr_t>(find.get());
    table.Finds.emplace(handle, std::move(find));
    return handle;
}

// Next entry for a handle of ours: 0, or -1 with ENOENT at the end; nullopt if it isn't ours.
template <typename Data>
std::optional<int> FindNextInTitle(intptr_t handle, Data* data)
{
    auto& table = Finds();
    const std::scoped_lock lock(table.Mutex);
    const auto it = table.Finds.find(handle);
    if (it == table.Finds.end())
    {
        return std::nullopt;
    }
    auto& find = *it->second;
    if (find.Next >= find.Entries.size())
    {
        SetErrno(ENOENT);
        return -1;
    }
    Fill(find.Entries.at(find.Next++), *data);
    return 0;
}

template <typename Data>
intptr_t WideFindFirst(const wchar_t* pattern, Data* data, const char* realName)
{
    const auto title = data == nullptr ? std::nullopt : InTitle(pattern);
    return title ? FindFirstInTitle(*title, data) : CallUcrt(realName, intptr_t{-1}, pattern, data);
}

template <typename Data>
intptr_t NarrowFindFirst(const char* pattern, Data* data, const char* realName)
{
    const auto title = data == nullptr || !Routes(pattern) ? std::nullopt : InTitle(Wide(pattern).c_str());
    return title ? FindFirstInTitle(*title, data) : CallUcrt(realName, intptr_t{-1}, pattern, data);
}

template <typename Data>
int FindNext(intptr_t handle, Data* data, const char* realName)
{
    const auto ours = data == nullptr ? std::nullopt : FindNextInTitle(handle, data);
    return ours ? *ours : CallUcrt(realName, -1, handle, data);
}

intptr_t WideFindFirst64i32Shim(const wchar_t* pattern, struct _wfinddata64i32_t* data) noexcept
{
    return GuardShim<intptr_t>("_wfindfirst64i32", -1, -1,
                               [=] { return WideFindFirst(pattern, data, "_wfindfirst64i32"); });
}

intptr_t FindFirst64i32Shim(const char* pattern, struct _finddata64i32_t* data) noexcept
{
    return GuardShim<intptr_t>("_findfirst64i32", -1, -1,
                               [=] { return NarrowFindFirst(pattern, data, "_findfirst64i32"); });
}

intptr_t WideFindFirst64Shim(const wchar_t* pattern, struct _wfinddata64_t* data) noexcept
{
    return GuardShim<intptr_t>("_wfindfirst64", -1, -1, [=] { return WideFindFirst(pattern, data, "_wfindfirst64"); });
}

intptr_t FindFirst64Shim(const char* pattern, struct __finddata64_t* data) noexcept
{
    return GuardShim<intptr_t>("_findfirst64", -1, -1, [=] { return NarrowFindFirst(pattern, data, "_findfirst64"); });
}

int WideFindNext64i32Shim(intptr_t handle, struct _wfinddata64i32_t* data) noexcept
{
    return GuardShim<int>("_wfindnext64i32", -1, -1, [=] { return FindNext(handle, data, "_wfindnext64i32"); });
}

int FindNext64i32Shim(intptr_t handle, struct _finddata64i32_t* data) noexcept
{
    return GuardShim<int>("_findnext64i32", -1, -1, [=] { return FindNext(handle, data, "_findnext64i32"); });
}

int WideFindNext64Shim(intptr_t handle, struct _wfinddata64_t* data) noexcept
{
    return GuardShim<int>("_wfindnext64", -1, -1, [=] { return FindNext(handle, data, "_wfindnext64"); });
}

int FindNext64Shim(intptr_t handle, struct __finddata64_t* data) noexcept
{
    return GuardShim<int>("_findnext64", -1, -1, [=] { return FindNext(handle, data, "_findnext64"); });
}

int FindCloseShim(intptr_t handle) noexcept
{
    return GuardShim<int>("_findclose", -1, -1,
                          [=]
                          {
                              auto& table = Finds();
                              {
                                  const std::scoped_lock lock(table.Mutex);
                                  if (table.Finds.erase(handle) != 0)
                                  {
                                      return 0;
                                  }
                              }
                              return CallUcrt("_findclose", -1, handle);
                          });
}

// --- std::_Fiopen (msvcp140) -------------------------------------------------------------------

constexpr const char* kFiopenWide = "?_Fiopen@std@@YAPEAU_iobuf@@PEB_WHH@Z";
constexpr const char* kFiopenShort = "?_Fiopen@std@@YAPEAU_iobuf@@PEBGHH@Z"; // wchar_t as unsigned short.
constexpr const char* kFiopenNarrow = "?_Fiopen@std@@YAPEAU_iobuf@@PEBDHH@Z";

FILE* StreamOpen(const wchar_t* path, int mode, int protection)
{
    // basic_filebuf::open's table: out alone and trunc replace the contents; in and app keep them.
    const bool writes = (mode & (kStreamOut | kStreamAppend)) != 0;
    const bool keeps = (mode & (kStreamIn | kStreamAppend)) != 0 && (mode & kStreamTruncate) == 0;
    const auto routed =
        OpenInTitle<FILE*>(path, writes, keeps, nullptr, [=](const wchar_t* host) noexcept
                           { return CallMsvcp(kFiopenWide, static_cast<FILE*>(nullptr), host, mode, protection); });
    return routed ? *routed : CallMsvcp(kFiopenWide, static_cast<FILE*>(nullptr), path, mode, protection);
}

FILE* FiopenWideShim(const wchar_t* path, int mode, int protection) noexcept
{
    return GuardShim<FILE*>("std::_Fiopen", nullptr, nullptr, [=] { return StreamOpen(path, mode, protection); });
}

FILE* FiopenShortShim(const unsigned short* path, int mode, int protection) noexcept
{
    return GuardShim<FILE*>(
        "std::_Fiopen", nullptr, nullptr,
        [=] { return StreamOpen(std::bit_cast<const wchar_t*>(std::bit_cast<uintptr_t>(path)), mode, protection); });
}

FILE* FiopenNarrowShim(const char* path, int mode, int protection) noexcept
{
    return GuardShim<FILE*>("std::_Fiopen", nullptr, nullptr,
                            [=]() -> FILE*
                            {
                                if (!Routes(path))
                                {
                                    return CallMsvcp(kFiopenNarrow, static_cast<FILE*>(nullptr), path, mode,
                                                     protection);
                                }
                                return StreamOpen(Wide(path).c_str(), mode, protection);
                            });
}
} // namespace

bool AddCrtFileExports(ShimModule& ucrtbase)
{
    return ucrtbase.Export("_wfopen", ToShimProc(&WideFopenShim)) && ucrtbase.Export("fopen", ToShimProc(&FopenShim)) &&
           ucrtbase.Export("_wfsopen", ToShimProc(&WideFsopenShim)) &&
           ucrtbase.Export("_fsopen", ToShimProc(&FsopenShim)) &&
           ucrtbase.Export("_wfopen_s", ToShimProc(&WideFopenSShim)) &&
           ucrtbase.Export("fopen_s", ToShimProc(&FopenSShim)) &&
           ucrtbase.Export("_wremove", ToShimProc(&WideRemoveShim)) &&
           ucrtbase.Export("remove", ToShimProc(&RemoveShim)) &&
           ucrtbase.Export("_wrename", ToShimProc(&WideRenameShim)) &&
           ucrtbase.Export("rename", ToShimProc(&RenameShim)) &&
           ucrtbase.Export("_wmkdir", ToShimProc(&WideMkdirShim)) &&
           ucrtbase.Export("_mkdir", ToShimProc(&MkdirShim)) &&
           ucrtbase.Export("_wrmdir", ToShimProc(&WideRmdirShim)) &&
           ucrtbase.Export("_rmdir", ToShimProc(&RmdirShim)) &&
           ucrtbase.Export("_waccess", ToShimProc(&WideAccessShim)) &&
           ucrtbase.Export("_access", ToShimProc(&AccessShim)) &&
           ucrtbase.Export("_wstat64", ToShimProc(&WideStat64Shim)) &&
           ucrtbase.Export("_stat64", ToShimProc(&Stat64Shim)) &&
           ucrtbase.Export("_wstat64i32", ToShimProc(&WideStat64i32Shim)) &&
           ucrtbase.Export("_stat64i32", ToShimProc(&Stat64i32Shim)) &&
           ucrtbase.Export("_wfindfirst64i32", ToShimProc(&WideFindFirst64i32Shim)) &&
           ucrtbase.Export("_findfirst64i32", ToShimProc(&FindFirst64i32Shim)) &&
           ucrtbase.Export("_wfindfirst64", ToShimProc(&WideFindFirst64Shim)) &&
           ucrtbase.Export("_findfirst64", ToShimProc(&FindFirst64Shim)) &&
           ucrtbase.Export("_wfindnext64i32", ToShimProc(&WideFindNext64i32Shim)) &&
           ucrtbase.Export("_findnext64i32", ToShimProc(&FindNext64i32Shim)) &&
           ucrtbase.Export("_wfindnext64", ToShimProc(&WideFindNext64Shim)) &&
           ucrtbase.Export("_findnext64", ToShimProc(&FindNext64Shim)) &&
           ucrtbase.Export("_findclose", ToShimProc(&FindCloseShim));
}

bool AddStlFileExports(ShimModule& msvcp140)
{
    return msvcp140.Export(kFiopenWide, ToShimProc(&FiopenWideShim)) &&
           msvcp140.Export(kFiopenShort, ToShimProc(&FiopenShortShim)) &&
           msvcp140.Export(kFiopenNarrow, ToShimProc(&FiopenNarrowShim));
}
} // namespace OpenEra::Kernel
