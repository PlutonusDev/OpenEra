// C runtime and C++ library file I/O test image, built with the DLL C runtime
// (/MD), so these calls go through ucrtbase / msvcp140 the way a title's do.
// Mapped and linked by ModuleManager in the tests; it's never loaded by the OS.
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include <Windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>

// The point is to call the C runtime's C APIs, so their lint rules don't apply here.
// NOLINTBEGIN(cppcoreguidelines-owning-memory,bugprone-exception-escape,cppcoreguidelines-pro-bounds-array-to-pointer-decay)
namespace
{
int ReadInto(FILE* file, char* out, int size) noexcept
{
    if (file == nullptr || out == nullptr || size <= 0)
    {
        return -1;
    }
    const size_t read = std::fread(out, 1, static_cast<size_t>(size - 1), file);
    out[read] = '\0'; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::fclose(file);
    return static_cast<int>(read);
}
} // namespace

extern "C" __declspec(dllexport) int LastErrno() noexcept
{
    return errno;
}

extern "C" __declspec(dllexport) int FopenWrite(const wchar_t* path, const char* text, const wchar_t* mode) noexcept
{
    FILE* const file = _wfopen(path, mode);
    if (file == nullptr)
    {
        return -1;
    }
    std::fputs(text, file);
    std::fclose(file);
    return 0;
}

extern "C" __declspec(dllexport) int NarrowFopenWrite(const char* path, const char* text) noexcept
{
    FILE* file = nullptr;
    if (fopen_s(&file, path, "w") != 0 || file == nullptr)
    {
        return -1;
    }
    std::fputs(text, file);
    std::fclose(file);
    return 0;
}

extern "C" __declspec(dllexport) int FopenRead(const wchar_t* path, char* out, int size) noexcept
{
    return ReadInto(_wfopen(path, L"rb"), out, size);
}

extern "C" __declspec(dllexport) int NarrowFopenRead(const char* path, char* out, int size) noexcept
{
    return ReadInto(std::fopen(path, "rb"), out, size); // NOLINT(cppcoreguidelines-owning-memory)
}

extern "C" __declspec(dllexport) int StreamWrite(const wchar_t* path, const char* text, int append) noexcept
{
    try
    {
        std::ofstream stream(path, append != 0 ? std::ios::app : std::ios::out);
        stream << text;
        return stream ? 0 : -1;
    }
    catch (...)
    {
        return -2;
    }
}

extern "C" __declspec(dllexport) int NarrowStreamWrite(const char* path, const char* text) noexcept
{
    try
    {
        std::ofstream stream(path);
        stream << text;
        return stream ? 0 : -1;
    }
    catch (...)
    {
        return -2;
    }
}

extern "C" __declspec(dllexport) int StreamRead(const wchar_t* path, char* out, int size) noexcept
{
    try
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return -1;
        }
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        strncpy_s(out, static_cast<size_t>(size), text.c_str(), _TRUNCATE);
        return static_cast<int>(text.size());
    }
    catch (...)
    {
        return -2;
    }
}

extern "C" __declspec(dllexport) int CrtRemove(const wchar_t* path) noexcept
{
    return _wremove(path);
}

extern "C" __declspec(dllexport) int CrtRename(const wchar_t* from, const wchar_t* to) noexcept
{
    return _wrename(from, to);
}

extern "C" __declspec(dllexport) int CrtMakeDirectory(const wchar_t* path) noexcept
{
    return _wmkdir(path);
}

extern "C" __declspec(dllexport) int CrtRemoveDirectory(const wchar_t* path) noexcept
{
    return _wrmdir(path);
}

extern "C" __declspec(dllexport) int CrtAccess(const wchar_t* path) noexcept
{
    return _waccess(path, 0);
}

extern "C" __declspec(dllexport) long long CrtStatSize(const wchar_t* path) noexcept
{
    struct _stat64 info{};
    return _wstat64(path, &info) == 0 ? info.st_size : -1;
}

extern "C" __declspec(dllexport) long long NarrowStatSize(const char* path) noexcept
{
    struct _stat64i32 info{};
    return _stat64i32(path, &info) == 0 ? info.st_size : -1;
}

// Names matched by a _wfindfirst pattern, joined with '|', or -1.
extern "C" __declspec(dllexport) int CrtFind(const wchar_t* pattern, wchar_t* out, int size) noexcept
{
    _wfinddata64i32_t data{};
    const intptr_t find = _wfindfirst64i32(pattern, &data);
    if (find == -1)
    {
        return -1;
    }
    std::wstring names;
    int count = 0;
    do
    {
        names += std::wstring(data.name) + L"|";
        ++count;
    } while (_wfindnext64i32(find, &data) == 0);
    _findclose(find);
    wcsncpy_s(out, static_cast<size_t>(size), names.c_str(), _TRUNCATE);
    return count;
}

extern "C" __declspec(dllexport) int NarrowCrtFindCount(const char* pattern) noexcept
{
    _finddata64i32_t data{};
    const intptr_t find = _findfirst64i32(pattern, &data);
    if (find == -1)
    {
        return -1;
    }
    int count = 0;
    do
    {
        ++count;
    } while (_findnext64i32(find, &data) == 0);
    _findclose(find);
    return count;
}

// std::filesystem's implementation is compiled into the image and calls Win32 itself.
extern "C" __declspec(dllexport) int DirectoryEntries(const wchar_t* path, wchar_t* out, int size) noexcept
{
    try
    {
        std::error_code error;
        std::wstring names;
        int count = 0;
        for (std::filesystem::directory_iterator it(path, error), end; !error && it != end; it.increment(error))
        {
            names += it->path().filename().wstring() + L"|";
            ++count;
        }
        wcsncpy_s(out, static_cast<size_t>(size), names.c_str(), _TRUNCATE);
        return error ? -1 : count;
    }
    catch (...)
    {
        return -2;
    }
}

extern "C" __declspec(dllexport) long long FilesystemRemoveAll(const wchar_t* path) noexcept
{
    std::error_code error;
    const auto removed = std::filesystem::remove_all(path, error);
    return error ? -1 : static_cast<long long>(removed);
}

extern "C" __declspec(dllexport) int FilesystemRename(const wchar_t* from, const wchar_t* to) noexcept
{
    std::error_code error;
    std::filesystem::rename(from, to, error);
    return error ? -1 : 0;
}

extern "C" __declspec(dllexport) int CreateFile2Write(const wchar_t* path, const char* text) noexcept
{
    HANDLE file = CreateFile2(path, GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return -1;
    }
    DWORD written = 0;
    const BOOL wrote = WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    CloseHandle(file);
    return wrote != FALSE ? 0 : -1;
}
// NOLINTEND(cppcoreguidelines-owning-memory,bugprone-exception-escape,cppcoreguidelines-pro-bounds-array-to-pointer-decay)
