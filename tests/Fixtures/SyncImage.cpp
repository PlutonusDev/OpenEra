// Synchronization, time and process test image. The tests map and link it with
// ModuleManager, so its Win32 calls go through OpenEra's import binding (shims,
// then host passthrough) exactly as a title's would. Each check function
// returns a bit mask of the checks that failed: 0 means everything passed.
#include <array>
#include <atomic>

#include <Windows.h>

namespace
{
constexpr int kThreads = 4;
constexpr int kIterations = 10'000;
constexpr ULONGLONG kYear2020FileTime = 132'223'104'000'000'000ULL; // 2020-01-01 in 100 ns units since 1601.

struct Counter
{
    CRITICAL_SECTION Section{};
    SRWLOCK Lock = SRWLOCK_INIT;
    long Value = 0;
    bool UseSrw = false;
};

DWORD WINAPI Increment(LPVOID parameter) noexcept
{
    auto& counter = *static_cast<Counter*>(parameter);
    for (int index = 0; index < kIterations; ++index)
    {
        if (counter.UseSrw)
        {
            AcquireSRWLockExclusive(&counter.Lock);
            ++counter.Value;
            ReleaseSRWLockExclusive(&counter.Lock);
        }
        else
        {
            EnterCriticalSection(&counter.Section);
            ++counter.Value;
            LeaveCriticalSection(&counter.Section);
        }
    }
    return 0;
}

// Runs kThreads incrementing threads and waits for all of them with one call.
bool RunCounter(Counter& counter, bool& waitedForAll) noexcept
{
    std::array<HANDLE, kThreads> threads{};
    for (auto& thread : threads)
    {
        thread = CreateThread(nullptr, 0, &Increment, &counter, 0, nullptr);
        if (thread == nullptr)
        {
            return false;
        }
    }
    waitedForAll = WaitForMultipleObjects(kThreads, threads.data(), TRUE, INFINITE) == WAIT_OBJECT_0;
    for (auto* const thread : threads)
    {
        CloseHandle(thread);
    }
    return counter.Value == kThreads * kIterations;
}

struct Signal
{
    HANDLE Event = nullptr;
    std::atomic<bool> Woken{false};
};

DWORD WINAPI WaitForSignal(LPVOID parameter) noexcept
{
    auto& signal = *static_cast<Signal*>(parameter);
    if (WaitForSingleObject(signal.Event, INFINITE) == WAIT_OBJECT_0)
    {
        signal.Woken = true;
    }
    return 0;
}

constexpr DWORD kWorkerExitCode = 0x5EED;
constexpr SIZE_T kWorkerStack = SIZE_T{1024} * 1024;

struct Worker
{
    DWORD Slot = 0;
    DWORD ThreadId = 0;
    bool SlotIsolated = false;
};

DWORD WINAPI RunWorker(LPVOID parameter) noexcept
{
    auto& worker = *static_cast<Worker*>(parameter);
    worker.ThreadId = GetCurrentThreadId();
    // A new thread's slot starts empty, and its value is its own.
    int local = 0;
    const bool startsEmpty = TlsGetValue(worker.Slot) == nullptr;
    TlsSetValue(worker.Slot, &local);
    worker.SlotIsolated = startsEmpty && TlsGetValue(worker.Slot) == &local;
    ExitThread(kWorkerExitCode);
}
} // namespace

extern "C" __declspec(dllexport) unsigned RunSyncChecks() noexcept
{
    unsigned failed = 0;

    Counter section;
    InitializeCriticalSection(&section.Section);
    bool waitedForAll = false;
    failed |= RunCounter(section, waitedForAll) ? 0U : 1U;
    failed |= waitedForAll ? 0U : 8U;
    DeleteCriticalSection(&section.Section);

    Counter srw;
    InitializeSRWLock(&srw.Lock);
    srw.UseSrw = true;
    failed |= RunCounter(srw, waitedForAll) ? 0U : 2U;

    // Manual-reset event: stays signalled until reset.
    auto* const manual = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetEvent(manual);
    const bool firstWait = WaitForSingleObject(manual, 0) == WAIT_OBJECT_0;
    const bool secondWait = WaitForSingleObject(manual, 0) == WAIT_OBJECT_0; // Still signalled.
    const bool signalled = firstWait && secondWait;
    ResetEvent(manual);
    failed |= signalled && WaitForSingleObject(manual, 0) == WAIT_TIMEOUT ? 0U : 4U;

    // Wait-any reports which handle fired.
    auto* const other = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    const std::array<HANDLE, 2> pair = {manual, other};
    failed |= WaitForMultipleObjects(2, pair.data(), FALSE, 0) == WAIT_OBJECT_0 + 1 ? 0U : 16U;
    CloseHandle(other);
    CloseHandle(manual);

    // Auto-reset event wakes a waiting thread.
    Signal signal;
    signal.Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto* const waiter = CreateThread(nullptr, 0, &WaitForSignal, &signal, 0, nullptr);
    Sleep(10);
    SetEvent(signal.Event);
    const bool joined = waiter != nullptr && WaitForSingleObject(waiter, 5'000) == WAIT_OBJECT_0;
    failed |= joined && signal.Woken ? 0U : 32U;
    if (waiter != nullptr)
    {
        CloseHandle(waiter);
    }
    CloseHandle(signal.Event);
    return failed;
}

extern "C" __declspec(dllexport) unsigned RunTimeChecks() noexcept
{
    unsigned failed = 0;

    LARGE_INTEGER frequency{};
    LARGE_INTEGER first{};
    LARGE_INTEGER second{};
    failed |= QueryPerformanceFrequency(&frequency) != FALSE && frequency.QuadPart > 0 ? 0U : 1U;
    QueryPerformanceCounter(&first);
    QueryPerformanceCounter(&second);
    failed |= second.QuadPart >= first.QuadPart ? 0U : 2U;

    const ULONGLONG ticksBefore = GetTickCount64();
    QueryPerformanceCounter(&first);
    Sleep(20);
    QueryPerformanceCounter(&second);
    failed |= GetTickCount64() - ticksBefore >= 10 ? 0U : 4U;
    const LONGLONG elapsedMs =
        frequency.QuadPart > 0 ? (second.QuadPart - first.QuadPart) * 1000 / frequency.QuadPart : 0;
    failed |= elapsedMs >= 10 && elapsedMs < 1'000 ? 0U : 64U;

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG nowValue = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    failed |= nowValue > kYear2020FileTime ? 0U : 8U;

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    failed |= info.dwNumberOfProcessors > 0 && info.dwPageSize == 4'096 ? 0U : 16U;

    SetLastError(1'234);
    failed |= GetLastError() == 1'234 ? 0U : 32U;
    return failed;
}

extern "C" __declspec(dllexport) unsigned RunProcessChecks() noexcept
{
    unsigned failed = 0;
    failed |= GetModuleHandleW(nullptr) != nullptr ? 0U : 1U;

    // The image finds itself through OpenEra's loader shims.
    const HMODULE self = GetModuleHandleW(L"SyncImage.dll");
    auto* const found = self == nullptr ? nullptr : GetProcAddress(self, "RunProcessChecks");
    failed |= found == reinterpret_cast<FARPROC>(&RunProcessChecks)
                  ? 0U
                  : 2U; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    failed |= LoadLibraryExW(L"SyncImage.dll", nullptr, 0) == self ? 0U : 4U;
    return failed;
}

extern "C" __declspec(dllexport) unsigned RunThreadChecks() noexcept
{
    unsigned failed = 0;

    // Threads, exit codes and TLS slots.
    Worker worker;
    worker.Slot = TlsAlloc();
    int mine = 0;
    TlsSetValue(worker.Slot, &mine);
    auto* const thread =
        CreateThread(nullptr, kWorkerStack, &RunWorker, &worker, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    DWORD exitCode = 0;
    const bool joined = thread != nullptr && WaitForSingleObject(thread, 5'000) == WAIT_OBJECT_0 &&
                        GetExitCodeThread(thread, &exitCode) != FALSE;
    failed |= joined && exitCode == kWorkerExitCode ? 0U : 1U;
    failed |= worker.ThreadId != 0 && worker.ThreadId != GetCurrentThreadId() ? 0U : 2U;
    failed |= worker.SlotIsolated && TlsGetValue(worker.Slot) == &mine ? 0U : 4U;
    failed |= TlsFree(worker.Slot) != FALSE ? 0U : 8U;

    // Priorities: an undocumented value is clamped instead of failing.
    auto* const self = GetCurrentThread();
    const int before = GetThreadPriority(self);
    failed |= SetThreadPriority(self, 5) != FALSE && GetThreadPriority(self) == THREAD_PRIORITY_HIGHEST ? 0U : 16U;
    failed |= SetThreadPriority(self, THREAD_PRIORITY_TIME_CRITICAL) != FALSE &&
                      GetThreadPriority(self) == THREAD_PRIORITY_TIME_CRITICAL
                  ? 0U
                  : 32U;
    SetThreadPriority(self, before);

    // Affinity: a console core this PC may not have still gets a processor.
    if (thread != nullptr)
    {
        CloseHandle(thread);
    }
    const DWORD_PTR original = SetThreadAffinityMask(self, DWORD_PTR{1} << 63);
    failed |= original != 0 ? 0U : 64U;
    SetThreadAffinityMask(self, original);

    // Names.
    PWSTR name = nullptr;
    const bool named = SUCCEEDED(SetThreadDescription(self, L"OpenEra test worker")) &&
                       SUCCEEDED(GetThreadDescription(self, &name)) && name != nullptr &&
                       lstrcmpW(name, L"OpenEra test worker") == 0;
    failed |= named ? 0U : 128U;
    LocalFree(name);
    SetThreadDescription(self, L"");

    // Yielding and sleeping.
    Sleep(1);
    failed |= SleepEx(1, TRUE) == 0 ? 0U : 256U;
    SwitchToThread();
    return failed;
}

extern "C" __declspec(dllexport) void SayHello() noexcept
{
    OutputDebugStringW(L"hello from a mapped image\r\n");
    OutputDebugStringA("narrow hello\n");
}
