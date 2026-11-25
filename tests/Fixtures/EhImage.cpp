// Exception-handling test image. The tests map and link it with ModuleManager;
// the OS loader never sees it, and its entry point is never called.
#include <Windows.h>

namespace
{
constexpr DWORD kTestExceptionCode = 0xE04F'4501;

void ThrowIfPositive(int value)
{
    if (value > 0)
    {
        throw value;
    }
}

int HandleTestException(DWORD code) noexcept
{
    return code == kTestExceptionCode ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

// Throws and catches inside the image: returns value + 1 when value > 0.
extern "C" __declspec(dllexport) int CatchInt(int value)
{
    try
    {
        ThrowIfPositive(value);
    }
    catch (const int caught)
    {
        return caught + 1;
    }
    return 0;
}

// Throws out of the image to the caller.
extern "C" __declspec(dllexport) void ThrowInt(int value)
{
    ThrowIfPositive(value);
}

// Raises a structured exception and handles it: returns 7.
extern "C" __declspec(dllexport) int RaiseAndHandle()
{
    __try
    {
        RaiseException(kTestExceptionCode, 0, 0, nullptr);
    }
    __except (HandleTestException(GetExceptionCode()))
    {
        return 7;
    }
    return 0;
}
