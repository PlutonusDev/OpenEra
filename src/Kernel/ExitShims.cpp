#include "ExitShims.h"

#include <atomic>
#include <bit>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>

#include "Kernel/ShimModule.h"

namespace OpenEra::Kernel
{
namespace
{
struct ExitState
{
    std::atomic<bool> Armed{false};
    std::atomic<bool> Requested{false};
    std::atomic<uint32_t> Code{0};
    HANDLE Event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // The title's C runtime hooks, kept here so the host never calls into its image.
    std::mutex Mutex;
    std::vector<void (*)()> AtExit;
    std::vector<void (*)()> AtQuickExit;
    void (*ThreadLocalCallback)(void*, unsigned long, void*) = nullptr;
    std::atomic<bool> Exiting{false}; // Handlers run once, by the first exit.
};

ExitState& State() noexcept
{
    static ExitState state;
    return state;
}

using CrtExit = void(int) noexcept;
using AtExitFunction = void();
using ThreadLocalCallback = void(void*, unsigned long, void*);

// An export of the host C runtime, for calls made while no title runs.
template <typename Function>
Function* HostProc(const char* name) noexcept
{
    const HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    auto* const proc = ucrt == nullptr ? nullptr : GetProcAddress(ucrt, name);
    return std::bit_cast<Function*>(std::bit_cast<uintptr_t>(proc));
}

enum class ExitKind : uint8_t
{
    Normal,    // exit
    Quick,     // quick_exit
    Immediate, // _exit
};

// Runs the title's handlers for `kind`, last registered first.
void RunTitleHandlers(ExitKind kind) noexcept
{
    auto& state = State();
    if (kind == ExitKind::Immediate || state.Exiting.exchange(true))
    {
        return;
    }
    std::vector<AtExitFunction*> handlers;
    ThreadLocalCallback* threadLocal = nullptr;
    {
        const std::scoped_lock lock(state.Mutex);
        handlers.swap(kind == ExitKind::Normal ? state.AtExit : state.AtQuickExit);
        if (kind == ExitKind::Normal)
        {
            threadLocal = std::exchange(state.ThreadLocalCallback, nullptr);
        }
    }
    if (threadLocal != nullptr)
    {
        threadLocal(nullptr, DLL_PROCESS_DETACH, nullptr);
    }
    for (auto it = handlers.rbegin(); it != handlers.rend(); ++it)
    {
        (*it)();
    }
}

// Registers `function` with the title while armed, or the host otherwise.
int RegisterHandler(ExitKind kind, const char* hostName, AtExitFunction* function) noexcept
{
    auto& state = State();
    if (!state.Armed)
    {
        auto* const host = HostProc<int(AtExitFunction*) noexcept>(hostName);
        return host == nullptr ? -1 : host(function);
    }
    try
    {
        const std::scoped_lock lock(state.Mutex);
        (kind == ExitKind::Quick ? state.AtQuickExit : state.AtExit).push_back(function);
        return 0;
    }
    catch (...)
    {
        return -1;
    }
}

int CrtAtExitShim(AtExitFunction* function) noexcept
{
    return RegisterHandler(ExitKind::Normal, "_crt_atexit", function);
}

int CrtAtQuickExitShim(AtExitFunction* function) noexcept
{
    return RegisterHandler(ExitKind::Quick, "_crt_at_quick_exit", function);
}

void RegisterThreadLocalExeAtExitCallbackShim(ThreadLocalCallback* callback) noexcept
{
    auto& state = State();
    if (state.Armed)
    {
        const std::scoped_lock lock(state.Mutex);
        state.ThreadLocalCallback = callback;
    }
    else if (auto* const host =
                 HostProc<void(ThreadLocalCallback*) noexcept>("_register_thread_local_exe_atexit_callback"))
    {
        host(callback);
    }
}

// The title's math error handler isn't installed in the host; math errors get the default handling.
void SetUserMathErrShim(void* handler) noexcept
{
    if (!State().Armed)
    {
        if (auto* const host = HostProc<void(void*) noexcept>("__setusermatherr"))
        {
            host(handler);
        }
    }
}

[[noreturn]] void EndTitleThread(uint32_t exitCode) noexcept
{
    ExitThread(exitCode);
}

[[noreturn]] void WINAPI ExitProcessShim(UINT exitCode) noexcept
{
    if (RequestTitleExit(exitCode))
    {
        EndTitleThread(exitCode);
    }
    ExitProcess(exitCode);
}

BOOL WINAPI TerminateProcessShim(HANDLE process, UINT exitCode) noexcept
{
    const bool current = process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId();
    if (current && RequestTitleExit(exitCode))
    {
        EndTitleThread(exitCode);
    }
    return TerminateProcess(process, exitCode);
}

// exit, _exit and quick_exit: the same request, with the C runtime's int code.
[[noreturn]] void ExitThroughCrt(const char* name, int exitCode) noexcept
{
    const auto code = std::bit_cast<uint32_t>(exitCode);
    if (State().Armed)
    {
        const std::string_view function(name);
        auto kind = ExitKind::Immediate;
        if (function == "exit")
        {
            kind = ExitKind::Normal;
        }
        else if (function == "quick_exit")
        {
            kind = ExitKind::Quick;
        }
        RunTitleHandlers(kind);
    }
    if (RequestTitleExit(code))
    {
        EndTitleThread(code);
    }
    if (auto* const real = HostProc<CrtExit>(name))
    {
        real(exitCode);
    }
    ExitProcess(code);
}

[[noreturn]] void ExitShim(int exitCode) noexcept
{
    ExitThroughCrt("exit", exitCode);
}

[[noreturn]] void UnderscoreExitShim(int exitCode) noexcept
{
    ExitThroughCrt("_exit", exitCode);
}

[[noreturn]] void QuickExitShim(int exitCode) noexcept
{
    ExitThroughCrt("quick_exit", exitCode);
}
} // namespace

void ArmTitleExit() noexcept
{
    auto& state = State();
    state.Requested = false;
    state.Code = 0;
    state.Exiting = false;
    {
        const std::scoped_lock lock(state.Mutex);
        state.AtExit.clear();
        state.AtQuickExit.clear();
        state.ThreadLocalCallback = nullptr;
    }
    ResetEvent(state.Event);
    state.Armed = true;
}

void DisarmTitleExit() noexcept
{
    State().Armed = false;
}

bool RequestTitleExit(uint32_t exitCode) noexcept
{
    auto& state = State();
    if (!state.Armed)
    {
        return false;
    }
    if (!state.Requested.exchange(true))
    {
        state.Code = exitCode;
        SetEvent(state.Event);
    }
    return true;
}

std::optional<uint32_t> TitleExitCode() noexcept
{
    const auto& state = State();
    return state.Requested ? std::optional<uint32_t>(state.Code.load()) : std::nullopt;
}

void* TitleExitEvent() noexcept
{
    return State().Event;
}

bool AddExitExports(ShimModule& kernel32)
{
    return kernel32.Export("ExitProcess", ToShimProc(&ExitProcessShim)) &&
           kernel32.Export("TerminateProcess", ToShimProc(&TerminateProcessShim));
}

bool AddCrtExitExports(ShimModule& ucrtbase)
{
    return ucrtbase.Export("exit", ToShimProc(&ExitShim)) &&
           ucrtbase.Export("_exit", ToShimProc(&UnderscoreExitShim)) &&
           ucrtbase.Export("quick_exit", ToShimProc(&QuickExitShim)) &&
           ucrtbase.Export("_crt_atexit", ToShimProc(&CrtAtExitShim)) &&
           ucrtbase.Export("_crt_at_quick_exit", ToShimProc(&CrtAtQuickExitShim)) &&
           ucrtbase.Export("_register_thread_local_exe_atexit_callback",
                           ToShimProc(&RegisterThreadLocalExeAtExitCallbackShim)) &&
           ucrtbase.Export("__setusermatherr", ToShimProc(&SetUserMathErrShim));
}
} // namespace OpenEra::Kernel
