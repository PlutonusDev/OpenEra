// Process-exit test executable, built without the C runtime. How it ends depends
// on its command line (which OpenEra supplies):
//   (nothing)        ExitProcess(7) from the main thread
//   --from-thread    ExitProcess(9) from another thread, while the main thread waits forever
//   --terminate      TerminateProcess(GetCurrentProcess(), 11)
//   --linger         returns 5, leaving a thread that never ends
#include <Windows.h>

namespace
{
// `needle` appears in `text`.
bool Contains(const wchar_t* text, const wchar_t* needle) noexcept
{
    const int length = lstrlenW(needle);
    for (; *text != L'\0'; ++text) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    {
        if (CompareStringOrdinal(text, length, needle, length, FALSE) == CSTR_EQUAL)
        {
            return true;
        }
    }
    return false;
}

DWORD WINAPI ExitFromThread(LPVOID /*parameter*/) noexcept
{
    Sleep(50);
    ExitProcess(9);
}

DWORD WINAPI Linger(LPVOID /*parameter*/) noexcept
{
    Sleep(INFINITE);
    return 0;
}
} // namespace

extern "C" DWORD WINAPI ExitImageStart(LPVOID /*parameter*/) noexcept
{
    const wchar_t* const commandLine = GetCommandLineW();
    if (Contains(commandLine, L"--from-thread"))
    {
        CloseHandle(CreateThread(nullptr, 0, &ExitFromThread, nullptr, 0, nullptr));
        Sleep(INFINITE);
    }
    if (Contains(commandLine, L"--terminate"))
    {
        TerminateProcess(GetCurrentProcess(), 11);
    }
    if (Contains(commandLine, L"--linger"))
    {
        CloseHandle(CreateThread(nullptr, 0, &Linger, nullptr, 0, nullptr));
        return 5;
    }
    ExitProcess(7);
}
