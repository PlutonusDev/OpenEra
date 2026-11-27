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
// Adds exit, _exit and quick_exit to a ucrtbase shim module. The title's atexit
// handlers aren't run: OpenEra's process carries on, so the C runtime mustn't shut down.
bool AddCrtExitExports(ShimModule& ucrtbase);
} // namespace OpenEra::Kernel
