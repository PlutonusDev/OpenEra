// Start-up test image: static TLS, a TLS callback and DllMain. The tests map
// and start it with ModuleManager and ImageInitializer, never the OS loader.
#include <atomic>

#include <Windows.h>

namespace
{
struct Counts
{
    std::atomic<int> ProcessAttach{0};
    std::atomic<int> ProcessDetach{0};
    std::atomic<int> ThreadAttach{0};
    std::atomic<int> CallbackProcessAttach{0};
    std::atomic<int> CallbackThreadAttach{0};
};

Counts& Get() noexcept
{
    static Counts counts;
    return counts;
}

// Initialised data, so it lives in the TLS template.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
thread_local int t_counter = 5;

void NTAPI OnTls(PVOID /*module*/, DWORD reason, PVOID /*reserved*/) noexcept
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ++Get().CallbackProcessAttach;
    }
    else if (reason == DLL_THREAD_ATTACH)
    {
        ++Get().CallbackThreadAttach;
    }
}
} // namespace

// Registers OnTls in the image's TLS callback array (the .CRT$XL* sections).
#pragma section(".CRT$XLB", long, read)
extern "C" __declspec(allocate(".CRT$XLB")) void(NTAPI* const kTestTlsCallback)(PVOID, DWORD, PVOID) = OnTls;
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:kTestTlsCallback")

// NOLINTNEXTLINE(misc-use-internal-linkage): the OS loader calls it.
BOOL WINAPI DllMain(HINSTANCE /*instance*/, DWORD reason, LPVOID /*reserved*/) noexcept
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ++Get().ProcessAttach;
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        ++Get().ProcessDetach;
    }
    else if (reason == DLL_THREAD_ATTACH)
    {
        ++Get().ThreadAttach;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) int NextCounter() noexcept
{
    return ++t_counter;
}

extern "C" __declspec(dllexport) int ProcessAttachCount() noexcept
{
    return Get().ProcessAttach.load();
}

extern "C" __declspec(dllexport) int ProcessDetachCount() noexcept
{
    return Get().ProcessDetach.load();
}

extern "C" __declspec(dllexport) int ThreadAttachCount() noexcept
{
    return Get().ThreadAttach.load();
}

extern "C" __declspec(dllexport) int CallbackProcessAttachCount() noexcept
{
    return Get().CallbackProcessAttach.load();
}

extern "C" __declspec(dllexport) int CallbackThreadAttachCount() noexcept
{
    return Get().CallbackThreadAttach.load();
}
