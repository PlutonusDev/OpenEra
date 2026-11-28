// Delay-imports DelayTargetImage.dll and DelayMissingImage.dll (/DELAYLOAD), so
// both are bound by the linker's own helper on first call.
#include <Windows.h>
#include <delayimp.h>

extern "C" __declspec(dllimport) int DelayTargetValue() noexcept;
extern "C" __declspec(dllimport) int DelayMissingValue() noexcept;

namespace
{
DWORD FilterCode(DWORD code, DWORD* seen) noexcept
{
    *seen = code;
    return EXCEPTION_EXECUTE_HANDLER;
}
} // namespace

extern "C" __declspec(dllexport) int CallDelayTarget() noexcept
{
    return DelayTargetValue();
}

extern "C" __declspec(dllexport) BOOL UnloadDelayTarget() noexcept
{
    return __FUnloadDelayLoadedDLL2("DelayTargetImage.dll");
}

// The exception code the first call to a missing delay-loaded DLL raises.
extern "C" __declspec(dllexport) DWORD CallDelayMissing() noexcept
{
    DWORD seen = 0;
    __try
    {
        DelayMissingValue();
    }
    __except (FilterCode(GetExceptionCode(), &seen))
    {
    }
    return seen;
}
