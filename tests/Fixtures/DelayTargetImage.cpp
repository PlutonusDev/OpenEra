// Delay-loaded by DelayImage.dll; counts its process attaches.
#include <atomic>

#include <Windows.h>

namespace
{
std::atomic<int>& Attaches() noexcept
{
    static std::atomic<int> attaches{0};
    return attaches;
}
} // namespace

extern "C" __declspec(dllexport) int DelayTargetAttaches() noexcept
{
    return Attaches().load();
}

extern "C" __declspec(dllexport) int DelayTargetValue() noexcept
{
    return 42;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): the OS loader calls it.
BOOL WINAPI DllMain(HINSTANCE /*instance*/, DWORD reason, LPVOID /*reserved*/) noexcept
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ++Attaches();
    }
    return TRUE;
}
