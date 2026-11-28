#include "TaskQueue.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Windows.h>

namespace OpenEra::Kernel::Gdk
{
namespace
{
struct Entry
{
    TaskQueueCallback* Callback = nullptr;
    void* Context = nullptr;
};

void Run(const Entry& entry, bool canceled) noexcept
{
    if (entry.Callback != nullptr)
    {
        entry.Callback(entry.Context, canceled);
    }
}
} // namespace

// One port: its queued callbacks and how they're dispatched.
class TaskPortObject
{
public:
    TaskPortObject(TaskQueueDispatchMode mode, TaskQueueObject& owner)
        : m_mode(mode)
        , m_owner(owner)
    {
    }

    [[nodiscard]] TaskQueueObject& Owner() const noexcept { return m_owner; }

    // Takes ownership of `entry` on success.
    int32_t Submit(const Entry& entry);
    bool Dispatch(uint32_t timeoutInMs);
    // Runs everything still queued as canceled, then waits out callbacks in flight.
    void Terminate();

private:
    void RunOne(Entry entry);
    static void CALLBACK PoolCallback(PTP_CALLBACK_INSTANCE instance, void* context) noexcept;
    void Drain();

    TaskQueueDispatchMode m_mode;
    TaskQueueObject& m_owner;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<Entry> m_pending;
    uint32_t m_active = 0;
    bool m_draining = false; // A serialized-thread-pool worker is running.
    bool m_terminated = false;
};

namespace
{
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
thread_local const TaskPortObject* t_runningPort = nullptr;

struct Monitor
{
    TaskQueueMonitorCallback* Callback = nullptr;
    void* Context = nullptr;
};

struct Delayed
{
    TaskQueueObject* Queue = nullptr;
    TaskQueuePort Port = TaskQueuePort::Work;
    Entry Callback;
    PTP_TIMER Timer = nullptr;
};

struct Waiter
{
    TaskQueueObject* Queue = nullptr;
    TaskQueuePort Port = TaskQueuePort::Work;
    Entry Callback;
    HANDLE Handle = nullptr;
    PTP_WAIT Wait = nullptr;
    std::unique_ptr<std::weak_ptr<Waiter>> Holder; // The wait callback's context.
    std::mutex Mutex;
    bool Registered = true;
};
} // namespace

class TaskQueueObject
{
public:
    TaskQueueObject(TaskQueueDispatchMode workMode, TaskQueueDispatchMode completionMode)
        : m_ownedWork(std::make_unique<TaskPortObject>(workMode, *this))
        , m_ownedCompletion(std::make_unique<TaskPortObject>(completionMode, *this))
        , m_work(m_ownedWork.get())
        , m_completion(m_ownedCompletion.get())
    {
    }

    TaskQueueObject(TaskPortObject& work, TaskPortObject& completion)
        : m_work(&work)
        , m_completion(&completion)
    {
        work.Owner().AddRef();
        completion.Owner().AddRef();
    }

    ~TaskQueueObject()
    {
        if (!m_ownedWork)
        {
            m_work->Owner().Release();
            m_completion->Owner().Release();
        }
    }

    TaskQueueObject(const TaskQueueObject&) = delete;
    TaskQueueObject& operator=(const TaskQueueObject&) = delete;
    TaskQueueObject(TaskQueueObject&&) = delete;
    TaskQueueObject& operator=(TaskQueueObject&&) = delete;

    void AddRef() noexcept { m_references.fetch_add(1); }
    void Release() noexcept
    {
        if (m_references.fetch_sub(1) == 1)
        {
            delete this; // NOLINT(cppcoreguidelines-owning-memory): handles are reference counted.
        }
    }

    [[nodiscard]] TaskPortObject& Port(TaskQueuePort port) noexcept
    {
        return port == TaskQueuePort::Work ? *m_work : *m_completion;
    }

    int32_t Submit(TaskQueuePort port, const Entry& entry);
    int32_t SubmitDelayed(TaskQueuePort port, uint32_t delayMs, const Entry& entry);
    int32_t RegisterWaiter(TaskQueuePort port, HANDLE handle, const Entry& entry, uint64_t& token);
    void UnregisterWaiter(uint64_t token);
    uint64_t RegisterMonitor(const Monitor& monitor);
    void UnregisterMonitor(uint64_t token);
    int32_t Terminate(bool wait, void* context, TaskQueueTerminatedCallback* callback);

private:
    static void CALLBACK OnTimer(PTP_CALLBACK_INSTANCE instance, void* context, PTP_TIMER timer) noexcept;
    static void CALLBACK OnWait(PTP_CALLBACK_INSTANCE instance, void* context, PTP_WAIT wait,
                                TP_WAIT_RESULT result) noexcept;
    static void WaiterFired(void* context, bool canceled) noexcept;
    void FinishTermination(void* context, TaskQueueTerminatedCallback* callback);
    static void CloseWaiter(Waiter& waiter) noexcept;

    std::atomic<uint32_t> m_references{1};
    std::unique_ptr<TaskPortObject> m_ownedWork; // Null for a composite queue.
    std::unique_ptr<TaskPortObject> m_ownedCompletion;
    TaskPortObject* m_work;
    TaskPortObject* m_completion;

    std::mutex m_mutex;
    bool m_terminated = false;
    uint64_t m_nextToken = 1;
    std::unordered_map<uint64_t, Monitor> m_monitors;
    std::vector<Delayed*> m_delayed; // Owned here until fired or canceled.
    std::unordered_map<uint64_t, std::shared_ptr<Waiter>> m_waiters;
};

int32_t TaskPortObject::Submit(const Entry& entry)
{
    std::unique_lock lock(m_mutex);
    if (m_terminated)
    {
        return kAbort;
    }
    if (m_mode == TaskQueueDispatchMode::Immediate)
    {
        ++m_active;
        lock.unlock();
        RunOne(entry);
        return kOk;
    }
    m_pending.push_back(entry);
    const bool startWorker = m_mode == TaskQueueDispatchMode::ThreadPool ||
                             (m_mode == TaskQueueDispatchMode::SerializedThreadPool && !m_draining);
    if (m_mode == TaskQueueDispatchMode::SerializedThreadPool)
    {
        m_draining = true;
    }
    lock.unlock();
    m_changed.notify_all();
    if (startWorker)
    {
        m_owner.AddRef();
        if (TrySubmitThreadpoolCallback(&PoolCallback, this, nullptr) == FALSE)
        {
            m_owner.Release();
            const std::scoped_lock relock(m_mutex);
            m_pending.pop_back();
            m_draining = false;
            return kOutOfMemory;
        }
    }
    return kOk;
}

void TaskPortObject::RunOne(Entry entry)
{
    // m_active was raised by the caller.
    const TaskPortObject* const previous = t_runningPort;
    t_runningPort = this;
    Run(entry, false);
    t_runningPort = previous;
    {
        const std::scoped_lock lock(m_mutex);
        --m_active;
    }
    m_changed.notify_all();
}

void TaskPortObject::Drain()
{
    std::unique_lock lock(m_mutex);
    while (!m_pending.empty())
    {
        const Entry entry = m_pending.front();
        m_pending.pop_front();
        ++m_active;
        lock.unlock();
        RunOne(entry);
        lock.lock();
        if (m_mode == TaskQueueDispatchMode::ThreadPool)
        {
            break; // One callback per pool work item.
        }
    }
    if (m_mode == TaskQueueDispatchMode::SerializedThreadPool)
    {
        m_draining = false;
    }
    lock.unlock();
    m_changed.notify_all();
}

void CALLBACK TaskPortObject::PoolCallback(PTP_CALLBACK_INSTANCE /*instance*/, void* context) noexcept
{
    auto* const port = static_cast<TaskPortObject*>(context);
    if (port == nullptr)
    {
        return;
    }
    try
    {
        port->Drain();
    }
    catch (...) // NOLINT(bugprone-empty-catch): a thread pool callback must not throw.
    {
    }
    port->m_owner.Release();
}

bool TaskPortObject::Dispatch(uint32_t timeoutInMs)
{
    std::unique_lock lock(m_mutex);
    const auto ready = [this] { return m_terminated || !m_pending.empty(); };
    if (timeoutInMs == INFINITE)
    {
        m_changed.wait(lock, ready);
    }
    else if (!m_changed.wait_for(lock, std::chrono::milliseconds(timeoutInMs), ready))
    {
        return false;
    }
    if (m_terminated || m_pending.empty())
    {
        return false;
    }
    const Entry entry = m_pending.front();
    m_pending.pop_front();
    ++m_active;
    lock.unlock();
    RunOne(entry);
    return true;
}

void TaskPortObject::Terminate()
{
    std::deque<Entry> canceled;
    {
        const std::scoped_lock lock(m_mutex);
        m_terminated = true;
        canceled.swap(m_pending);
    }
    m_changed.notify_all();
    for (const Entry& entry : canceled)
    {
        Run(entry, true);
    }
    // A callback on this port that terminates its own queue doesn't wait for itself.
    const uint32_t self = t_runningPort == this ? 1 : 0;
    std::unique_lock lock(m_mutex);
    m_changed.wait(lock, [this, self] { return m_active <= self && !m_draining; });
}

int32_t TaskQueueObject::Submit(TaskQueuePort port, const Entry& entry)
{
    std::vector<Monitor> monitors;
    {
        const std::scoped_lock lock(m_mutex);
        if (m_terminated)
        {
            return kAbort;
        }
        monitors.reserve(m_monitors.size());
        for (const auto& [token, monitor] : m_monitors)
        {
            monitors.push_back(monitor);
        }
    }
    if (const int32_t result = Port(port).Submit(entry); result != kOk)
    {
        return result;
    }
    for (const Monitor& monitor : monitors)
    {
        monitor.Callback(monitor.Context, this, port);
    }
    return kOk;
}

int32_t TaskQueueObject::SubmitDelayed(TaskQueuePort port, uint32_t delayMs, const Entry& entry)
{
    if (delayMs == 0)
    {
        return Submit(port, entry);
    }
    auto delayed = std::make_unique<Delayed>();
    delayed->Queue = this;
    delayed->Port = port;
    delayed->Callback = entry;
    delayed->Timer = CreateThreadpoolTimer(&OnTimer, delayed.get(), nullptr);
    if (delayed->Timer == nullptr)
    {
        return kOutOfMemory;
    }
    {
        const std::scoped_lock lock(m_mutex);
        if (m_terminated)
        {
            CloseThreadpoolTimer(delayed->Timer);
            return kAbort;
        }
        m_delayed.push_back(delayed.get());
    }
    AddRef();
    // Relative due times are negative, in 100 ns units.
    const int64_t delay = delayMs;
    const int64_t due = -delay * 10'000;
    FILETIME dueTime{};
    dueTime.dwLowDateTime = std::bit_cast<std::array<DWORD, 2>>(due).front();
    dueTime.dwHighDateTime = std::bit_cast<std::array<DWORD, 2>>(due).back();
    SetThreadpoolTimer(delayed.release()->Timer, &dueTime, 0, 0);
    return kOk;
}

void CALLBACK TaskQueueObject::OnTimer(PTP_CALLBACK_INSTANCE /*instance*/, void* context, PTP_TIMER /*timer*/) noexcept
{
    auto* const delayed = static_cast<Delayed*>(context);
    if (delayed == nullptr)
    {
        return;
    }
    TaskQueueObject* const queue = delayed->Queue;
    if (queue == nullptr)
    {
        return;
    }
    bool owned = false;
    {
        const std::scoped_lock lock(queue->m_mutex);
        // Whoever takes it off the list owns it; termination may already have.
        owned = std::erase(queue->m_delayed, delayed) != 0;
    }
    if (!owned)
    {
        return;
    }
    try
    {
        if (queue->Submit(delayed->Port, delayed->Callback) != kOk)
        {
            Run(delayed->Callback, true);
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch): a thread pool callback must not throw.
    {
    }
    CloseThreadpoolTimer(delayed->Timer); // Allowed from its own callback.
    std::unique_ptr<Delayed>{delayed}.reset();
    queue->Release();
}

int32_t TaskQueueObject::RegisterWaiter(TaskQueuePort port, HANDLE handle, const Entry& entry, uint64_t& token)
{
    auto waiter = std::make_shared<Waiter>();
    waiter->Queue = this;
    waiter->Port = port;
    waiter->Callback = entry;
    waiter->Handle = handle;
    waiter->Holder = std::make_unique<std::weak_ptr<Waiter>>(waiter);
    waiter->Wait = CreateThreadpoolWait(&OnWait, waiter->Holder.get(), nullptr);
    if (waiter->Wait == nullptr)
    {
        return kOutOfMemory;
    }
    {
        const std::scoped_lock lock(m_mutex);
        if (m_terminated)
        {
            CloseThreadpoolWait(waiter->Wait);
            return kAbort;
        }
        token = m_nextToken++;
        m_waiters.emplace(token, waiter);
    }
    SetThreadpoolWait(waiter->Wait, handle, nullptr);
    return kOk;
}

namespace
{
// Context of a waiter's queued callback.
struct WaiterCall
{
    std::shared_ptr<Waiter> Owner;
};
} // namespace

void CALLBACK TaskQueueObject::OnWait(PTP_CALLBACK_INSTANCE /*instance*/, void* context, PTP_WAIT /*wait*/,
                                      TP_WAIT_RESULT /*result*/) noexcept
{
    const auto* const holder = static_cast<std::weak_ptr<Waiter>*>(context);
    if (holder == nullptr)
    {
        return;
    }
    try
    {
        auto waiter = holder->lock();
        if (!waiter)
        {
            return;
        }
        auto call = std::make_unique<WaiterCall>(WaiterCall{.Owner = waiter});
        if (waiter->Queue->Submit(waiter->Port, {.Callback = &WaiterFired, .Context = call.get()}) == kOk)
        {
            std::ignore = call.release(); // WaiterFired frees it.
        }
    }
    catch (...) // NOLINT(bugprone-empty-catch): a thread pool callback must not throw.
    {
    }
}

void TaskQueueObject::WaiterFired(void* context, bool canceled) noexcept
{
    const std::unique_ptr<WaiterCall> call(static_cast<WaiterCall*>(context));
    if (!call || !call->Owner)
    {
        return;
    }
    Waiter& waiter = *call->Owner;
    {
        const std::scoped_lock lock(waiter.Mutex);
        if (!waiter.Registered)
        {
            return;
        }
    }
    Run(waiter.Callback, canceled);
    // Signalled handles that stay signalled (manual-reset events) fire again.
    const std::scoped_lock lock(waiter.Mutex);
    if (waiter.Registered && !canceled)
    {
        SetThreadpoolWait(waiter.Wait, waiter.Handle, nullptr);
    }
}

void TaskQueueObject::CloseWaiter(Waiter& waiter) noexcept
{
    {
        const std::scoped_lock lock(waiter.Mutex);
        waiter.Registered = false;
        SetThreadpoolWait(waiter.Wait, nullptr, nullptr);
    }
    WaitForThreadpoolWaitCallbacks(waiter.Wait, TRUE);
    CloseThreadpoolWait(waiter.Wait);
    waiter.Holder = nullptr;
}

void TaskQueueObject::UnregisterWaiter(uint64_t token)
{
    std::shared_ptr<Waiter> waiter;
    {
        const std::scoped_lock lock(m_mutex);
        const auto it = m_waiters.find(token);
        if (it == m_waiters.end())
        {
            return;
        }
        waiter = std::move(it->second);
        m_waiters.erase(it);
    }
    CloseWaiter(*waiter);
}

uint64_t TaskQueueObject::RegisterMonitor(const Monitor& monitor)
{
    const std::scoped_lock lock(m_mutex);
    const uint64_t token = m_nextToken++;
    m_monitors.emplace(token, monitor);
    return token;
}

void TaskQueueObject::UnregisterMonitor(uint64_t token)
{
    const std::scoped_lock lock(m_mutex);
    m_monitors.erase(token);
}

int32_t TaskQueueObject::Terminate(bool wait, void* context, TaskQueueTerminatedCallback* callback)
{
    {
        const std::scoped_lock lock(m_mutex);
        if (m_terminated)
        {
            return kOk;
        }
        m_terminated = true;
    }
    if (wait)
    {
        FinishTermination(context, callback);
        return kOk;
    }

    struct Pending
    {
        TaskQueueObject* Queue;
        void* Context;
        TaskQueueTerminatedCallback* Callback;
    };
    auto pending = std::make_unique<Pending>(Pending{.Queue = this, .Context = context, .Callback = callback});
    AddRef();
    const auto work = [](PTP_CALLBACK_INSTANCE /*instance*/, void* raw) noexcept
    {
        const std::unique_ptr<Pending> owned(static_cast<Pending*>(raw));
        if (!owned)
        {
            return;
        }
        try
        {
            owned->Queue->FinishTermination(owned->Context, owned->Callback);
        }
        catch (...) // NOLINT(bugprone-empty-catch): a thread pool callback must not throw.
        {
        }
        owned->Queue->Release();
    };
    if (TrySubmitThreadpoolCallback(work, pending.get(), nullptr) == FALSE)
    {
        Release();
        FinishTermination(context, callback);
        return kOk;
    }
    std::ignore = pending.release();
    return kOk;
}

void TaskQueueObject::FinishTermination(void* context, TaskQueueTerminatedCallback* callback)
{
    std::vector<Delayed*> delayed;
    std::unordered_map<uint64_t, std::shared_ptr<Waiter>> waiters;
    {
        const std::scoped_lock lock(m_mutex);
        delayed.swap(m_delayed);
        waiters.swap(m_waiters);
    }
    for (Delayed* const item : delayed)
    {
        SetThreadpoolTimer(item->Timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(item->Timer, TRUE);
        CloseThreadpoolTimer(item->Timer);
        Run(item->Callback, true);
        std::unique_ptr<Delayed>{item}.reset();
        Release(); // Each pending timer held a reference.
    }
    for (auto& [token, waiter] : waiters)
    {
        CloseWaiter(*waiter);
    }
    // Work first, then completion; a composite queue doesn't own (or end) its ports.
    if (m_ownedWork)
    {
        m_ownedWork->Terminate();
        m_ownedCompletion->Terminate();
    }
    if (callback != nullptr)
    {
        callback(context);
    }
}

namespace
{
std::mutex& ProcessMutex() noexcept
{
    static std::mutex mutex;
    return mutex;
}

struct ProcessQueue
{
    TaskQueueObject* Queue = nullptr;
    bool Set = false; // A title chose the queue (possibly none).
};

ProcessQueue& Process() noexcept
{
    static ProcessQueue process;
    return process;
}

template <typename Function>
int32_t Guarded(Function&& function) noexcept
{
    try
    {
        return std::forward<Function>(function)();
    }
    catch (const std::bad_alloc&)
    {
        return kOutOfMemory;
    }
    catch (...)
    {
        return std::bit_cast<int32_t>(0x8000'4005U); // E_FAIL
    }
}
} // namespace

int32_t TaskQueueCreate(TaskQueueDispatchMode workMode, TaskQueueDispatchMode completionMode,
                        TaskQueueHandle* queue) noexcept
{
    if (queue == nullptr || workMode > TaskQueueDispatchMode::Immediate ||
        completionMode > TaskQueueDispatchMode::Immediate)
    {
        return kInvalidArgument;
    }
    return Guarded(
        [&]
        {
            *queue = std::make_unique<TaskQueueObject>(workMode, completionMode).release();
            return kOk;
        });
}

int32_t TaskQueueCreateComposite(TaskQueuePortHandle workPort, TaskQueuePortHandle completionPort,
                                 TaskQueueHandle* queue) noexcept
{
    if (workPort == nullptr || completionPort == nullptr || queue == nullptr)
    {
        return kInvalidArgument;
    }
    return Guarded(
        [&]
        {
            *queue = std::make_unique<TaskQueueObject>(*workPort, *completionPort).release();
            return kOk;
        });
}

int32_t TaskQueueGetPort(TaskQueueHandle queue, TaskQueuePort port, TaskQueuePortHandle* portHandle) noexcept
{
    if (queue == nullptr || portHandle == nullptr || port > TaskQueuePort::Completion)
    {
        return kInvalidArgument;
    }
    *portHandle = &queue->Port(port);
    return kOk;
}

int32_t TaskQueueDuplicateHandle(TaskQueueHandle queue, TaskQueueHandle* duplicated) noexcept
{
    if (queue == nullptr || duplicated == nullptr)
    {
        return kInvalidArgument;
    }
    queue->AddRef();
    *duplicated = queue;
    return kOk;
}

bool TaskQueueDispatch(TaskQueueHandle queue, TaskQueuePort port, uint32_t timeoutInMs) noexcept
{
    if (queue == nullptr || port > TaskQueuePort::Completion)
    {
        return false;
    }
    try
    {
        return queue->Port(port).Dispatch(timeoutInMs);
    }
    catch (...)
    {
        return false;
    }
}

void TaskQueueCloseHandle(TaskQueueHandle queue) noexcept
{
    if (queue != nullptr)
    {
        queue->Release();
    }
}

int32_t TaskQueueTerminate(TaskQueueHandle queue, bool wait, void* callbackContext,
                           TaskQueueTerminatedCallback* callback) noexcept
{
    if (queue == nullptr)
    {
        return kInvalidArgument;
    }
    return Guarded([&] { return queue->Terminate(wait, callbackContext, callback); });
}

int32_t TaskQueueSubmitCallback(TaskQueueHandle queue, TaskQueuePort port, void* callbackContext,
                                TaskQueueCallback* callback) noexcept
{
    return TaskQueueSubmitDelayedCallback(queue, port, 0, callbackContext, callback);
}

int32_t TaskQueueSubmitDelayedCallback(TaskQueueHandle queue, TaskQueuePort port, uint32_t delayMs,
                                       void* callbackContext, TaskQueueCallback* callback) noexcept
{
    if (callback == nullptr || port > TaskQueuePort::Completion)
    {
        return kInvalidArgument;
    }
    TaskQueueObject* const target = TaskQueueOrProcess(queue);
    if (target == nullptr)
    {
        return kInvalidArgument;
    }
    const int32_t result = Guarded(
        [&] { return target->SubmitDelayed(port, delayMs, {.Callback = callback, .Context = callbackContext}); });
    target->Release();
    return result;
}

int32_t TaskQueueRegisterWaiter(TaskQueueHandle queue, TaskQueuePort port, void* waitHandle, void* callbackContext,
                                TaskQueueCallback* callback, TaskQueueRegistrationToken* token) noexcept
{
    if (queue == nullptr || waitHandle == nullptr || callback == nullptr || token == nullptr ||
        port > TaskQueuePort::Completion)
    {
        return kInvalidArgument;
    }
    return Guarded(
        [&]
        {
            return queue->RegisterWaiter(port, waitHandle, {.Callback = callback, .Context = callbackContext},
                                         token->Token);
        });
}

void TaskQueueUnregisterWaiter(TaskQueueHandle queue, TaskQueueRegistrationToken token) noexcept
{
    if (queue != nullptr)
    {
        std::ignore = Guarded(
            [&]
            {
                queue->UnregisterWaiter(token.Token);
                return kOk;
            });
    }
}

int32_t TaskQueueRegisterMonitor(TaskQueueHandle queue, void* callbackContext, TaskQueueMonitorCallback* callback,
                                 TaskQueueRegistrationToken* token) noexcept
{
    if (queue == nullptr || callback == nullptr || token == nullptr)
    {
        return kInvalidArgument;
    }
    return Guarded(
        [&]
        {
            token->Token = queue->RegisterMonitor({.Callback = callback, .Context = callbackContext});
            return kOk;
        });
}

void TaskQueueUnregisterMonitor(TaskQueueHandle queue, TaskQueueRegistrationToken token) noexcept
{
    if (queue != nullptr)
    {
        std::ignore = Guarded(
            [&]
            {
                queue->UnregisterMonitor(token.Token);
                return kOk;
            });
    }
}

TaskQueueHandle TaskQueueOrProcess(TaskQueueHandle queue) noexcept
{
    if (queue != nullptr)
    {
        queue->AddRef();
        return queue;
    }
    TaskQueueHandle process = nullptr;
    return TaskQueueGetCurrentProcessTaskQueue(&process) ? process : nullptr;
}

bool TaskQueueGetCurrentProcessTaskQueue(TaskQueueHandle* queue) noexcept
{
    if (queue == nullptr)
    {
        return false;
    }
    *queue = nullptr;
    const std::scoped_lock lock(ProcessMutex());
    auto& process = Process();
    if (process.Queue == nullptr && !process.Set &&
        TaskQueueCreate(TaskQueueDispatchMode::ThreadPool, TaskQueueDispatchMode::ThreadPool, &process.Queue) != kOk)
    {
        return false;
    }
    if (process.Queue == nullptr)
    {
        return false;
    }
    process.Queue->AddRef();
    *queue = process.Queue;
    return true;
}

void TaskQueueSetCurrentProcessTaskQueue(TaskQueueHandle queue) noexcept
{
    if (queue != nullptr)
    {
        queue->AddRef();
    }
    TaskQueueObject* previous = nullptr;
    {
        const std::scoped_lock lock(ProcessMutex());
        auto& process = Process();
        previous = std::exchange(process.Queue, queue);
        process.Set = true;
    }
    if (previous != nullptr)
    {
        previous->Release();
    }
}
} // namespace OpenEra::Kernel::Gdk
