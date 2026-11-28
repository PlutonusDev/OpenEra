#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include "Loader/ModuleManager.h"

namespace OpenEra::Core
{
class Logger;
} // namespace OpenEra::Core

namespace OpenEra::Loader
{
// Starts and stops title modules the way the OS loader would: static TLS, TLS
// callbacks, then DllMain, plus thread attach and detach for every thread
// created afterwards.
//
// Each module with TLS gets a patched copy of OpenEraTlsSlot.dll (written to
// `workDirectory`) whose template is the module's own, so the OS allocates and
// frees its per-thread blocks, including for threads that already exist.
// Copy and move are deleted by ModuleInitializer (redeclaring them here is C26456).
class ImageInitializer final : public ModuleInitializer // NOLINT(cppcoreguidelines-special-member-functions)
{
public:
    ImageInitializer(std::filesystem::path tlsSlot, std::filesystem::path workDirectory, Core::Logger& logger);
    ~ImageInitializer() override;

    [[nodiscard]] bool Attach(const LoadedModule& module) override;
    void Detach(const LoadedModule& module) override;
    // Waits out thread notifications still running in it, and frees its TLS slot.
    void Unloading(const LoadedModule& module) noexcept override;

    // Runs an executable's entry point on a new thread with the stack its
    // headers ask for, and returns its exit code once it finishes.
    [[nodiscard]] std::optional<uint32_t> RunEntryPoint(const LoadedModule& module);
    // RunEntryPoint also returns when a title thread ends "the process"
    // (Kernel::RequestTitleExit), with that exit code; a main thread still
    // running then is stopped, as the process would have stopped it.

    // Threads started since modules were attached that haven't ended (other
    // than the caller): title threads, and host threads started on their behalf.
    [[nodiscard]] size_t RunningThreadCount() const;
    // Leaves the TLS slots loaded on destruction, for threads that are still running.
    void KeepLoaded() noexcept;

    // OpenEraTlsSlot.dll next to the running executable.
    [[nodiscard]] static std::filesystem::path DefaultTlsSlotPath();

private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace OpenEra::Loader
