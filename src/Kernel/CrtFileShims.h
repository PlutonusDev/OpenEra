#pragma once

#include <string_view>

namespace OpenEra::Kernel
{
class ShimModule;

// fopen-style modes: whether the file is opened for writing, and whether its
// current contents are kept ("a", "r+") rather than replaced ("w").
[[nodiscard]] constexpr bool OpensForWrite(std::wstring_view mode) noexcept
{
    const auto flags = mode.substr(0, mode.find(L',')); // After a comma: ccs=... encoding options.
    return flags.find_first_of(L"wa+") != std::wstring_view::npos;
}

[[nodiscard]] constexpr bool KeepsContents(std::wstring_view mode) noexcept
{
    const auto flags = mode.substr(0, mode.find(L','));
    return flags.find(L'w') == std::wstring_view::npos;
}

// Adds the C runtime's path-taking file functions to a ucrtbase shim module:
// fopen, _wfopen, _fsopen, _wfsopen, fopen_s, _wfopen_s, remove, _wremove,
// rename, _wrename, _mkdir, _wmkdir, _rmdir, _wrmdir, _access, _waccess, the
// _stat64 / _stat64i32 families, and _findfirst / _findnext (64 and 64i32,
// narrow and wide) with _findclose. Title paths go through the title file
// layer, then to the host's own ucrtbase with the resolved path; others pass
// through unchanged.
bool AddCrtFileExports(ShimModule& ucrtbase);

// Adds std::_Fiopen (behind std::basic_filebuf::open, and so the file streams)
// to an msvcp140 shim module, routed the same way.
bool AddStlFileExports(ShimModule& msvcp140);
} // namespace OpenEra::Kernel
