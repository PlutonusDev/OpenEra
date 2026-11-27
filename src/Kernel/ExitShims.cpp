#include "ExitShims.h"

#include <atomic>
#include <bit>

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
};

ExitState& State() noexcept
{
    static ExitState state;
    return state;
}

using CrtExit = void(int) noexcept;

// The host C runtime's own exit functions, for calls made while no title runs.
CrtExit* HostExit(const char* name) noexcept
{
    const HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    auto* const proc = ucrt == nullptr ? nullptr : GetProcAddress(ucrt, name);
    return std::bit_cast<CrtExit*>(std::bit_cast<uintptr_t>(proc));
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
    if (RequestTitleExit(code))
    {
        EndTitleThread(code);
    }
    if (auto* const real = HostExit(name))
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
           ucrtbase.Export("quick_exit", ToShimProc(&QuickExitShim));
}
} // namespace OpenEra::Kernel
