#pragma once

// Routing shared by the kernel32 and C runtime file shims (internal to Kernel):
// how a path inside the title folder maps onto the folder and its overlay.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <Windows.h>

#include "Kernel/TitleFileSystem.h"

namespace OpenEra::Kernel::FileRouting
{
struct TitlePath
{
    std::reference_wrapper<TitleFileSystem> Files;
    std::filesystem::path Relative;
};

// The active title file system and `name` relative to its folder, if it's inside.
[[nodiscard]] std::optional<TitlePath> InTitle(const wchar_t* name);

// ANSI code page conversions, as the narrow Win32 and C runtime functions use.
[[nodiscard]] std::wstring FromAnsi(const char* text);
[[nodiscard]] std::string ToAnsi(std::wstring_view text);

// A host path to use instead, or the Win32 error to report.
struct Routed
{
    std::optional<std::wstring> HostPath;
    DWORD Error = ERROR_SUCCESS;
};

// Read: the overlay's copy if there is one, else the folder's; deleted paths fail.
[[nodiscard]] Routed ForRead(const TitlePath& path);
// Write: the overlay path, with the folder's file copied there first unless the
// contents are being replaced.
[[nodiscard]] Routed ForWrite(const TitlePath& path, bool copyExisting);

// Each returns ERROR_SUCCESS or a Win32 error code.
[[nodiscard]] DWORD Delete(const TitlePath& path);
[[nodiscard]] DWORD MakeDirectory(const TitlePath& path, LPSECURITY_ATTRIBUTES security);
[[nodiscard]] DWORD RemoveDirectoryAt(const TitlePath& path);
// Moves or renames when `from` or `to` is in the title folder (MoveFileExW flags).
[[nodiscard]] DWORD Move(const wchar_t* from, const wchar_t* to, DWORD flags);

// Everything a FindFirstFile pattern matches across both layers: overlay
// entries win, and deleted and internal names are left out.
[[nodiscard]] std::vector<WIN32_FIND_DATAW> Matches(const TitlePath& pattern, FINDEX_INFO_LEVELS level,
                                                    FINDEX_SEARCH_OPS search, DWORD flags);

// Handles opened in the overlay for a title path with DELETE access, so a delete
// through the handle (disposition or delete-on-close) also hides the folder's file.
void TrackHandle(HANDLE handle, const TitlePath& path, bool deleteOnClose);
// ERROR_DIR_NOT_EMPTY for a tracked directory that still has visible entries
// (its overlay copy may be empty while the folder's isn't); else ERROR_SUCCESS.
[[nodiscard]] DWORD CanDeleteThrough(HANDLE handle);
void HandleDeleted(HANDLE handle);
void HandleClosed(HANDLE handle);

[[nodiscard]] DWORD ErrorFor(const std::error_code& error) noexcept;
} // namespace OpenEra::Kernel::FileRouting
