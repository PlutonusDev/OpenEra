#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>

namespace OpenEra::Kernel
{
// The title folder as a title sees it, without ever modifying it: reads come
// from an overlay copy if there is one, otherwise from the folder; writes,
// new files and directories go to the overlay (copying a file there on its
// first write); deletes remove the overlay copy and hide the folder's file.
// Hidden paths are kept in the overlay, so they survive restarts.
//
// Paths are compared ASCII case-insensitively, like the file system itself
// for the names titles use. "Relative" paths here are relative to the title folder.
class TitleFileSystem
{
public:
    TitleFileSystem(const std::filesystem::path& titleRoot, const std::filesystem::path& overlayRoot);

    [[nodiscard]] const std::filesystem::path& TitleRoot() const noexcept { return m_titleRoot; }
    [[nodiscard]] const std::filesystem::path& OverlayRoot() const noexcept { return m_overlayRoot; }

    // `path` (absolute, `\\?\`-prefixed, or relative to the current directory)
    // relative to the title folder, or nullopt if it's outside.
    [[nodiscard]] std::optional<std::filesystem::path> TitleRelative(const std::filesystem::path& path) const;

    [[nodiscard]] bool IsDeleted(const std::filesystem::path& relative) const;
    [[nodiscard]] bool Exists(const std::filesystem::path& relative) const;

    // Where to read `relative` from, or nullopt if it was deleted.
    [[nodiscard]] std::optional<std::filesystem::path> ReadPath(const std::filesystem::path& relative) const;

    // Overlay path to write `relative` at, with its parent directories created.
    // With `copyExisting`, the folder's file (or directory) is copied there first
    // unless it was deleted. Nullopt if the overlay can't be prepared.
    [[nodiscard]] std::optional<std::filesystem::path> WritePath(const std::filesystem::path& relative,
                                                                 bool copyExisting);

    // Fails with no_such_file_or_directory for a missing or already deleted path.
    [[nodiscard]] std::error_code Delete(const std::filesystem::path& relative);
    // Un-hides `relative` once something new was created there.
    void Undelete(const std::filesystem::path& relative);
    // Hides the folder's `relative` (if it has one) without touching the overlay,
    // for deletes that already happened there (by handle, or by moving it away).
    void Hide(const std::filesystem::path& relative);

    // Whether the overlay has its own copy of `relative`.
    [[nodiscard]] bool InOverlay(const std::filesystem::path& relative) const;
    // A directory with nothing visible in it, in either layer.
    [[nodiscard]] bool IsEmptyDirectory(const std::filesystem::path& relative) const;
    // Removes an empty directory: the overlay's copy, and hides the folder's.
    // Fails with no_such_file_or_directory, not_a_directory or directory_not_empty.
    [[nodiscard]] std::error_code RemoveEmptyDirectory(const std::filesystem::path& relative);

    // Overlay files OpenEra keeps for itself, which listings skip.
    [[nodiscard]] static bool IsInternal(const std::filesystem::path& relative);

    // File system the file shims use while a title runs.
    static void SetActive(TitleFileSystem* fileSystem) noexcept;
    [[nodiscard]] static TitleFileSystem* Active() noexcept;

private:
    [[nodiscard]] static std::wstring Key(const std::filesystem::path& relative);
    void SaveDeleted() const;

    std::filesystem::path m_titleRoot;
    std::filesystem::path m_overlayRoot;
    mutable std::mutex m_mutex;
    std::unordered_set<std::wstring> m_deleted; // Keys of hidden paths.
};
} // namespace OpenEra::Kernel
