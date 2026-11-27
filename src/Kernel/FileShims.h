#pragma once

namespace OpenEra::Kernel
{
class ShimModule;

// Adds file shims to a kernel32 shim module. Paths inside the title folder go
// through TitleFileSystem::Active() (reads from the folder or its overlay,
// writes to the overlay); everything else passes through unchanged:
// - opening: CreateFileW / CreateFileA / CreateFile2;
// - attributes and listings: GetFileAttributes{W,A}, GetFileAttributesExW,
//   FindFirstFile{W,ExW}, FindNextFileW, FindClose;
// - changes: CreateDirectory{W,A}, RemoveDirectory{W,A}, DeleteFile{W,A},
//   MoveFile{W,A,ExW,ExA}, CopyFile{W,A,ExW} and CopyFile2;
// - deletes through handles: SetFileInformationByHandle (disposition) and
//   CloseHandle (delete-on-close), which std::filesystem::remove relies on.
// Other handle-based calls (ReadFile, WriteFile, GetFileSizeEx,
// SetFilePointerEx) need no shim and pass through to the host.
bool AddFileExports(ShimModule& kernel32);
} // namespace OpenEra::Kernel
