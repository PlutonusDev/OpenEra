#pragma once

namespace OpenEra::Kernel
{
class ShimModule;

// Adds file shims to a kernel32 shim module. Paths inside the title folder go
// through TitleFileSystem::Active() (reads from the folder or its overlay,
// writes to the overlay); everything else passes through unchanged:
// CreateFileW, GetFileAttributesW, GetFileAttributesExW, FindFirstFileW,
// FindFirstFileExW, FindNextFileW, FindClose, CreateDirectoryW and DeleteFileW.
// Handle-based calls (ReadFile, WriteFile, CloseHandle, GetFileSizeEx,
// SetFilePointerEx) need no shim and pass through to the host.
bool AddFileExports(ShimModule& kernel32);
} // namespace OpenEra::Kernel
