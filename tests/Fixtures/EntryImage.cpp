// Entry-point test executable, built without the C runtime so returning from
// the entry point doesn't exit the process.
#include <Windows.h>

extern "C" DWORD WINAPI EntryImageStart(LPVOID /*parameter*/) noexcept
{
    return 42;
}
