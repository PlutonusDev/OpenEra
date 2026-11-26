// OpenEraTlsSlot.dll: a static TLS slot for an image OpenEra maps itself.
// ImageInitializer loads a copy whose TLS template has been replaced with a
// title module's, so the OS loader gives every thread a block for it, then
// points the module at this copy's TLS index. The DLL also forwards thread
// attach and detach notifications.
#include <atomic>

#include <Windows.h>

using ThreadNotify = void (*)(unsigned long reason) noexcept;

extern "C"
{
    // Template capacity. Never accessed: copies carry a title module's data here.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,readability-identifier-naming)
    __declspec(thread) unsigned char OpenEraTlsSlotTemplate[0x1'0000] = {1};
}
#pragma comment(linker, "/INCLUDE:OpenEraTlsSlotTemplate")
#pragma comment(linker, "/INCLUDE:_tls_used") // The TLS directory, which nothing else references.

namespace
{
std::atomic<ThreadNotify>& Notify() noexcept
{
    static std::atomic<ThreadNotify> notify{nullptr};
    return notify;
}
} // namespace

extern "C" __declspec(dllexport) void OpenEraTlsSlotSetNotify(ThreadNotify notify) noexcept
{
    Notify().store(notify);
}

// NOLINTNEXTLINE(misc-use-internal-linkage): the OS loader calls it.
BOOL WINAPI DllMain(HINSTANCE /*instance*/, DWORD reason, LPVOID /*reserved*/) noexcept
{
    if (reason == DLL_THREAD_ATTACH || reason == DLL_THREAD_DETACH)
    {
        if (const ThreadNotify notify = Notify().load(); notify != nullptr)
        {
            notify(reason);
        }
    }
    return TRUE;
}
