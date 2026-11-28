#pragma once

#include <cstdint>
#include <optional>

namespace OpenEra::Kernel
{
class ShimModule;

// While armed (a title is running), a title that ends "the process" only ends
// the calling thread: the exit code is recorded and TitleExitEvent signalled,
// so OpenEra can shut the title down, log its summary and report the code.
void ArmTitleExit() noexcept;
void DisarmTitleExit() noexcept;

// Records the first request while armed; false (nothing recorded) when disarmed.
[[nodiscard]] bool RequestTitleExit(uint32_t exitCode) noexcept;
[[nodiscard]] std::optional<uint32_t> TitleExitCode() noexcept;
// A manual-reset event (a HANDLE), signalled when the title asks to exit.
[[nodiscard]] void* TitleExitEvent() noexcept;

// Adds ExitProcess and TerminateProcess (for the current process) to a kernel32 shim module.
bool AddExitExports(ShimModule& kernel32);
// Adds exit, _exit and quick_exit to a ucrtbase shim module, and the hooks an
// executable's C runtime start-up registers with the process-wide C runtime
// (_crt_atexit, _crt_at_quick_exit, _register_thread_local_exe_atexit_callback,
// __setusermatherr). While armed those stay with the title, not the host, and
// the title's exit runs its TLS destructors and atexit handlers (quick_exit its
// at_quick_exit ones) before ending its thread; the host's C runtime keeps running.
bool AddCrtExitExports(ShimModule& ucrtbase);
} // namespace OpenEra::Kernel
