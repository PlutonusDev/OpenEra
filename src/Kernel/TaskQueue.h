#pragma once

#include <bit>
#include <cstdint>

namespace OpenEra::Kernel::Gdk
{
// The GDK's XTaskQueue API, written from its public documentation. The
// functions use the documented signatures, so they're exported as-is.
enum class TaskQueueDispatchMode : uint32_t // NOLINT(performance-enum-size): ABI type.
{
    Manual,
    ThreadPool,
    SerializedThreadPool,
    Immediate,
};

enum class TaskQueuePort : uint32_t // NOLINT(performance-enum-size): ABI type.
{
    Work,
    Completion,
};

class TaskQueueObject;
class TaskPortObject;
using TaskQueueHandle = TaskQueueObject*;
using TaskQueuePortHandle = TaskPortObject*;

using TaskQueueCallback = void(void* context, bool canceled);
using TaskQueueTerminatedCallback = void(void* context);
using TaskQueueMonitorCallback = void(void* context, TaskQueueHandle queue, TaskQueuePort port);

struct TaskQueueRegistrationToken
{
    uint64_t Token;
};
static_assert(sizeof(TaskQueueRegistrationToken) == 8);

// HRESULTs the queue reports.
constexpr int32_t kOk = 0;
constexpr int32_t kAbort = std::bit_cast<int32_t>(0x8000'4004U);
constexpr int32_t kInvalidArgument = std::bit_cast<int32_t>(0x8007'0057U);
constexpr int32_t kOutOfMemory = std::bit_cast<int32_t>(0x8007'000EU);

int32_t TaskQueueCreate(TaskQueueDispatchMode workMode, TaskQueueDispatchMode completionMode,
                        TaskQueueHandle* queue) noexcept;
int32_t TaskQueueCreateComposite(TaskQueuePortHandle workPort, TaskQueuePortHandle completionPort,
                                 TaskQueueHandle* queue) noexcept;
int32_t TaskQueueGetPort(TaskQueueHandle queue, TaskQueuePort port, TaskQueuePortHandle* portHandle) noexcept;
int32_t TaskQueueDuplicateHandle(TaskQueueHandle queue, TaskQueueHandle* duplicated) noexcept;
bool TaskQueueDispatch(TaskQueueHandle queue, TaskQueuePort port, uint32_t timeoutInMs) noexcept;
void TaskQueueCloseHandle(TaskQueueHandle queue) noexcept;
int32_t TaskQueueTerminate(TaskQueueHandle queue, bool wait, void* callbackContext,
                           TaskQueueTerminatedCallback* callback) noexcept;
int32_t TaskQueueSubmitCallback(TaskQueueHandle queue, TaskQueuePort port, void* callbackContext,
                                TaskQueueCallback* callback) noexcept;
int32_t TaskQueueSubmitDelayedCallback(TaskQueueHandle queue, TaskQueuePort port, uint32_t delayMs,
                                       void* callbackContext, TaskQueueCallback* callback) noexcept;
int32_t TaskQueueRegisterWaiter(TaskQueueHandle queue, TaskQueuePort port, void* waitHandle, void* callbackContext,
                                TaskQueueCallback* callback, TaskQueueRegistrationToken* token) noexcept;
void TaskQueueUnregisterWaiter(TaskQueueHandle queue, TaskQueueRegistrationToken token) noexcept;
int32_t TaskQueueRegisterMonitor(TaskQueueHandle queue, void* callbackContext, TaskQueueMonitorCallback* callback,
                                 TaskQueueRegistrationToken* token) noexcept;
void TaskQueueUnregisterMonitor(TaskQueueHandle queue, TaskQueueRegistrationToken token) noexcept;
// The process queue is created on first use (thread pool on both ports) until a title sets its own.
bool TaskQueueGetCurrentProcessTaskQueue(TaskQueueHandle* queue) noexcept;
void TaskQueueSetCurrentProcessTaskQueue(TaskQueueHandle queue) noexcept;

// `queue` itself with a new reference, or the process queue when null; nullptr if neither exists.
[[nodiscard]] TaskQueueHandle TaskQueueOrProcess(TaskQueueHandle queue) noexcept;
} // namespace OpenEra::Kernel::Gdk
