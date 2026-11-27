// Process-identity test executable, built without the C runtime. Run by the
// launcher tests with the arguments `--level "two words"`; returns a bit mask of
// the checks that failed (0 when the title sees itself as the process image).
#include <array>

#include <Windows.h>

namespace
{
constexpr DWORD kBufferSize = 1024;
using Buffer = std::array<wchar_t, kBufferSize>;

// Static, so they need no zeroing code (there's no memset without the C runtime).
Buffer& ModuleBuffer() noexcept
{
    static Buffer buffer;
    return buffer;
}

Buffer& DirectoryBuffer() noexcept
{
    static Buffer buffer;
    return buffer;
}

Buffer& ImageBuffer() noexcept
{
    static Buffer buffer;
    return buffer;
}

bool EqualsIgnoreCase(const wchar_t* left, const wchar_t* right) noexcept
{
    return CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}

// `text` starts with `prefix` (ignoring case); returns what follows, or null.
const wchar_t* AfterPrefix(const wchar_t* text, const wchar_t* prefix) noexcept
{
    const int length = lstrlenW(prefix);
    if (lstrlenW(text) < length || CompareStringOrdinal(text, length, prefix, length, TRUE) != CSTR_EQUAL)
    {
        return nullptr;
    }
    return text + length; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}
} // namespace

extern "C" DWORD WINAPI IdentityImageStart(LPVOID /*parameter*/) noexcept
{
    DWORD failed = 0;
    wchar_t* const module = ModuleBuffer().data();
    wchar_t* const directory = DirectoryBuffer().data();
    wchar_t* const image = ImageBuffer().data();

    // The module path is the title's executable, inside the current directory (the title folder).
    const DWORD moduleLength = GetModuleFileNameW(nullptr, module, kBufferSize);
    const DWORD directoryLength = GetCurrentDirectoryW(kBufferSize, directory);
    const wchar_t* const rest = directoryLength == 0 ? nullptr : AfterPrefix(module, directory);
    if (moduleLength == 0 || rest == nullptr || !EqualsIgnoreCase(rest, L"\\IdentityImage.exe"))
    {
        failed |= 1;
    }

    // The command line is "<module>" followed by the arguments.
    const wchar_t* command = AfterPrefix(GetCommandLineW(), L"\"");
    command = command == nullptr ? nullptr : AfterPrefix(command, module);
    if (command == nullptr || !EqualsIgnoreCase(command, L"\" --level \"two words\""))
    {
        failed |= 2;
    }

    // The process image name is the same executable.
    DWORD imageLength = kBufferSize;
    if (QueryFullProcessImageNameW(GetCurrentProcess(), 0, image, &imageLength) == FALSE ||
        !EqualsIgnoreCase(image, module))
    {
        failed |= 4;
    }

    // A buffer too small for the path is filled and reported as truncated.
    std::array<wchar_t, 4> tiny{L'x', L'x', L'x', L'x'};
    if (GetModuleFileNameW(nullptr, tiny.data(), 4) != 4 || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        tiny.back() != L'\0')
    {
        failed |= 8;
    }
    return failed;
}
