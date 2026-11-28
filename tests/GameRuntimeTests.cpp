// The Gaming Runtime shims: XTaskQueue, XAsync and the XGameRuntime / XSystem
// queries, called the way a title calls them.
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

#include "Kernel/Async.h"
#include "Kernel/GameRuntimeShims.h"
#include "Kernel/ShimModule.h"
#include "Kernel/TaskQueue.h"

using namespace OpenEra::Kernel;
using namespace OpenEra::Kernel::Gdk;

namespace
{
using Mode = TaskQueueDispatchMode;
using Port = TaskQueuePort;

// A queue closed at the end of the test.
class Queue
{
public:
    Queue(Mode work, Mode completion) { REQUIRE(TaskQueueCreate(work, completion, &m_handle) == kOk); }
    explicit Queue(TaskQueueHandle handle) noexcept
        : m_handle(handle)
    {
    }
    ~Queue() { TaskQueueCloseHandle(m_handle); }
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    Queue(Queue&&) = delete;
    Queue& operator=(Queue&&) = delete;

    [[nodiscard]] TaskQueueHandle Get() const noexcept { return m_handle; }

private:
    TaskQueueHandle m_handle = nullptr;
};

struct Log
{
    std::mutex Mutex;
    std::vector<std::string> Events;

    void Add(std::string event)
    {
        const std::scoped_lock lock(Mutex);
        Events.push_back(std::move(event));
    }
};

struct Tagged
{
    Log* Target;
    const char* Name;
};

void Record(void* context, bool canceled)
{
    const auto* tagged = static_cast<Tagged*>(context);
    tagged->Target->Add(std::string(tagged->Name) + (canceled ? " canceled" : ""));
}

void Count(void* context, bool /*canceled*/)
{
    ++*static_cast<std::atomic<int>*>(context);
}

// Waits up to a second for `condition`.
template <typename Condition>
bool Eventually(Condition condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition())
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
} // namespace

TEST_CASE("Manual queues run callbacks in order when dispatched", "[gdk][taskqueue]")
{
    const Queue queue(Mode::Manual, Mode::Manual);
    Log log;
    std::array<Tagged, 3> tags{
        {{.Target = &log, .Name = "a"}, {.Target = &log, .Name = "b"}, {.Target = &log, .Name = "c"}}};
    for (auto& tag : tags)
    {
        REQUIRE(TaskQueueSubmitCallback(queue.Get(), Port::Work, &tag, &Record) == kOk);
    }
    Tagged done{.Target = &log, .Name = "done"};
    REQUIRE(TaskQueueSubmitCallback(queue.Get(), Port::Completion, &done, &Record) == kOk);

    CHECK(log.Events.empty());
    CHECK(TaskQueueDispatch(queue.Get(), Port::Work, 0));
    CHECK(log.Events == std::vector<std::string>{"a"});
    while (TaskQueueDispatch(queue.Get(), Port::Work, 0))
    {
    }
    CHECK(log.Events == std::vector<std::string>{"a", "b", "c"});
    CHECK(TaskQueueDispatch(queue.Get(), Port::Completion, 0));
    CHECK(log.Events.back() == "done");
    CHECK_FALSE(TaskQueueDispatch(queue.Get(), Port::Completion, 10)); // Times out empty.
}

TEST_CASE("Thread pool, serialized and immediate ports dispatch themselves", "[gdk][taskqueue]")
{
    SECTION("thread pool")
    {
        const Queue queue(Mode::ThreadPool, Mode::ThreadPool);
        std::atomic<int> count{0};
        for (int index = 0; index < 50; ++index)
        {
            REQUIRE(TaskQueueSubmitCallback(queue.Get(), Port::Work, &count, &Count) == kOk);
        }
        CHECK(Eventually([&] { return count == 50; }));
    }

    SECTION("serialized thread pool runs one at a time, in order")
    {
        const Queue queue(Mode::SerializedThreadPool, Mode::Manual);
        struct State
        {
            std::atomic<int> Running{0};
            std::atomic<int> MaxRunning{0};
            std::mutex Mutex;
            std::vector<int> Order;
        } state;
        struct Item
        {
            State* Shared;
            int Index;
        };
        std::vector<Item> items;
        items.reserve(20);
        for (int index = 0; index < 20; ++index)
        {
            items.push_back({.Shared = &state, .Index = index});
        }
        const auto run = [](void* context, bool /*canceled*/)
        {
            auto* item = static_cast<Item*>(context);
            const int running = ++item->Shared->Running;
            int seen = item->Shared->MaxRunning;
            while (running > seen && !item->Shared->MaxRunning.compare_exchange_weak(seen, running))
            {
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            {
                const std::scoped_lock lock(item->Shared->Mutex);
                item->Shared->Order.push_back(item->Index);
            }
            --item->Shared->Running;
        };
        for (auto& item : items)
        {
            REQUIRE(TaskQueueSubmitCallback(queue.Get(), Port::Work, &item, run) == kOk);
        }
        CHECK(Eventually(
            [&]
            {
                const std::scoped_lock lock(state.Mutex);
                return state.Order.size() == 20;
            }));
        CHECK(state.MaxRunning == 1);
        for (int index = 0; index < 20; ++index)
        {
            CHECK(state.Order.at(static_cast<size_t>(index)) == index);
        }
    }

    SECTION("immediate runs on the submitting thread before returning")
    {
        const Queue queue(Mode::Immediate, Mode::Immediate);
        struct Seen
        {
            DWORD Thread = 0;
        } seen;
        const auto run = [](void* context, bool /*canceled*/)
        { static_cast<Seen*>(context)->Thread = GetCurrentThreadId(); };
        REQUIRE(TaskQueueSubmitCallback(queue.Get(), Port::Completion, &seen, run) == kOk);
        CHECK(seen.Thread == GetCurrentThreadId());
    }
}

TEST_CASE("Delayed callbacks arrive after their delay", "[gdk][taskqueue]")
{
    const Queue queue(Mode::Manual, Mode::Manual);
    std::atomic<int> count{0};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(TaskQueueSubmitDelayedCallback(queue.Get(), Port::Work, 50, &count, &Count) == kOk);
    CHECK_FALSE(TaskQueueDispatch(queue.Get(), Port::Work, 0));
    CHECK(TaskQueueDispatch(queue.Get(), Port::Work, 5'000));
    CHECK(count == 1);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(40));
}

TEST_CASE("Terminating a queue cancels what's queued, then reports", "[gdk][taskqueue]")
{
    const Queue queue(Mode::Manual, Mode::Manual);
    Log log;
    Tagged work{.Target = &log, .Name = "work"};
    Tagged completion{.Target = &log, .Name = "completion"};
    Tagged delayed{.Target = &log, .Name = "delayed"};
    REQUIRE(TaskQueueSubmitCallback(queue.Get(), Port::Completion, &completion, &Record) == kOk);
    REQUIRE(TaskQueueSubmitCallback(queue.Get(), Port::Work, &work, &Record) == kOk);
    REQUIRE(TaskQueueSubmitDelayedCallback(queue.Get(), Port::Work, 60'000, &delayed, &Record) == kOk);
    const auto terminated = [](void* context) { static_cast<Log*>(context)->Add("terminated"); };

    REQUIRE(TaskQueueTerminate(queue.Get(), true, &log, terminated) == kOk);
    CHECK(log.Events ==
          std::vector<std::string>{"delayed canceled", "work canceled", "completion canceled", "terminated"});
    std::atomic<int> count{0};
    CHECK(TaskQueueSubmitCallback(queue.Get(), Port::Work, &count, &Count) == kAbort);
    CHECK_FALSE(TaskQueueDispatch(queue.Get(), Port::Work, INFINITE)); // Returns at once.

    SECTION("asynchronously, the callback still arrives")
    {
        const Queue other(Mode::ThreadPool, Mode::ThreadPool);
        std::atomic<int> done{0};
        const auto finished = [](void* context) { ++*static_cast<std::atomic<int>*>(context); };
        REQUIRE(TaskQueueTerminate(other.Get(), false, &done, finished) == kOk);
        CHECK(Eventually([&] { return done == 1; }));
    }
}

TEST_CASE("Monitors see each submission", "[gdk][taskqueue]")
{
    const Queue queue(Mode::Manual, Mode::Manual);
    struct Seen
    {
        TaskQueueHandle Queue = nullptr;
        std::vector<Port> Ports;
    } seen;
    const auto monitor = [](void* context, TaskQueueHandle handle, Port port)
    {
        auto* state = static_cast<Seen*>(context);
        state->Queue = handle;
        state->Ports.push_back(port);
    };
    TaskQueueRegistrationToken token{};
    REQUIRE(TaskQueueRegisterMonitor(queue.Get(), &seen, monitor, &token) == kOk);
    std::atomic<int> count{0};
    REQUIRE(TaskQueueSubmitCallback(queue.Get(), Port::Work, &count, &Count) == kOk);
    REQUIRE(TaskQueueSubmitCallback(queue.Get(), Port::Completion, &count, &Count) == kOk);
    CHECK(seen.Queue == queue.Get());
    CHECK(seen.Ports == std::vector<Port>{Port::Work, Port::Completion});

    TaskQueueUnregisterMonitor(queue.Get(), token);
    REQUIRE(TaskQueueSubmitCallback(queue.Get(), Port::Work, &count, &Count) == kOk);
    CHECK(seen.Ports.size() == 2);
}

TEST_CASE("Waiters run when their handle is signalled", "[gdk][taskqueue]")
{
    const Queue queue(Mode::Manual, Mode::Manual);
    auto* const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(event != nullptr);
    std::atomic<int> count{0};
    TaskQueueRegistrationToken token{};
    REQUIRE(TaskQueueRegisterWaiter(queue.Get(), Port::Work, event, &count, &Count, &token) == kOk);
    CHECK_FALSE(TaskQueueDispatch(queue.Get(), Port::Work, 20));

    SetEvent(event);
    CHECK(TaskQueueDispatch(queue.Get(), Port::Work, 5'000));
    CHECK(count == 1);
    SetEvent(event); // Re-armed after the callback.
    CHECK(TaskQueueDispatch(queue.Get(), Port::Work, 5'000));
    CHECK(count == 2);

    TaskQueueUnregisterWaiter(queue.Get(), token);
    SetEvent(event);
    CHECK_FALSE(TaskQueueDispatch(queue.Get(), Port::Work, 50));
    CloseHandle(event);
}

TEST_CASE("Composite queues and duplicated handles share ports", "[gdk][taskqueue]")
{
    TaskQueueHandle original = nullptr;
    REQUIRE(TaskQueueCreate(Mode::Manual, Mode::Manual, &original) == kOk);
    TaskQueuePortHandle work = nullptr;
    TaskQueuePortHandle completion = nullptr;
    REQUIRE(TaskQueueGetPort(original, Port::Work, &work) == kOk);
    REQUIRE(TaskQueueGetPort(original, Port::Completion, &completion) == kOk);
    TaskQueueHandle composite = nullptr;
    // Swapped on purpose: the composite's work port is the original's completion port.
    // NOLINTNEXTLINE(readability-suspicious-call-argument)
    REQUIRE(TaskQueueCreateComposite(completion, work, &composite) == kOk);
    TaskQueueHandle duplicate = nullptr;
    REQUIRE(TaskQueueDuplicateHandle(original, &duplicate) == kOk);
    TaskQueueCloseHandle(original); // The duplicate and the composite keep it alive.

    std::atomic<int> count{0};
    REQUIRE(TaskQueueSubmitCallback(composite, Port::Work, &count, &Count) == kOk);
    CHECK_FALSE(TaskQueueDispatch(duplicate, Port::Work, 0));
    CHECK(TaskQueueDispatch(duplicate, Port::Completion, 0));
    CHECK(count == 1);

    TaskQueueCloseHandle(composite);
    TaskQueueCloseHandle(duplicate);
}

TEST_CASE("The process task queue can be replaced and cleared", "[gdk][taskqueue]")
{
    TaskQueueHandle initial = nullptr;
    REQUIRE(TaskQueueGetCurrentProcessTaskQueue(&initial));
    CHECK(initial != nullptr);

    const Queue mine(Mode::Manual, Mode::Manual);
    TaskQueueSetCurrentProcessTaskQueue(mine.Get());
    TaskQueueHandle current = nullptr;
    CHECK(TaskQueueGetCurrentProcessTaskQueue(&current));
    CHECK(current == mine.Get());
    TaskQueueCloseHandle(current);

    // Null queues fall back to the process queue.
    std::atomic<int> count{0};
    REQUIRE(TaskQueueSubmitCallback(nullptr, Port::Work, &count, &Count) == kOk);
    CHECK(TaskQueueDispatch(mine.Get(), Port::Work, 0));
    CHECK(count == 1);

    TaskQueueSetCurrentProcessTaskQueue(nullptr);
    CHECK_FALSE(TaskQueueGetCurrentProcessTaskQueue(&current));
    CHECK(current == nullptr);
    CHECK(TaskQueueSubmitCallback(nullptr, Port::Work, &count, &Count) == kInvalidArgument);

    TaskQueueSetCurrentProcessTaskQueue(initial);
    TaskQueueCloseHandle(initial);
}

namespace
{
// An XAsync provider that records its ops and returns a 4-byte payload.
struct Provider
{
    std::vector<AsyncOp> Ops;
    bool CompleteOnWork = true;
    uint32_t Payload = 0xC0FFEE;
    int Cleanups = 0;

    // NOLINTNEXTLINE(bugprone-exception-escape): a test provider; recording an op only allocates.
    static int32_t Call(AsyncOp op, const AsyncProviderData* data) noexcept
    {
        auto* self = static_cast<Provider*>(data->Context);
        self->Ops.push_back(op);
        switch (op)
        {
        case AsyncOp::Begin:
            return AsyncSchedule(data->Async, 0);
        case AsyncOp::DoWork:
            if (!self->CompleteOnWork)
            {
                return kPending;
            }
            AsyncComplete(data->Async, kOk, sizeof(self->Payload));
            return kOk;
        case AsyncOp::GetResult:
            std::memcpy(data->Buffer, &self->Payload, sizeof(self->Payload));
            return kOk;
        case AsyncOp::Cancel:
            return kOk;
        case AsyncOp::Cleanup:
            ++self->Cleanups;
            return kOk;
        }
        return kOk;
    }
};

const int kIdentity = 0;
} // namespace

TEST_CASE("XAsync calls run their work on the queue and return a result", "[gdk][xasync]")
{
    const Queue queue(Mode::Manual, Mode::Manual);
    Provider provider;
    struct Completed
    {
        std::atomic<int> Count{0};
        int32_t Status = 0;
    } completed;
    AsyncBlock block{};
    block.Queue = queue.Get();
    block.Context = &completed;
    block.Callback = [](AsyncBlock* async)
    {
        auto* state = static_cast<Completed*>(async->Context);
        state->Status = AsyncGetStatus(async, false);
        ++state->Count;
    };

    REQUIRE(AsyncBegin(&block, &provider, &kIdentity, "Test", &Provider::Call) == kOk);
    CHECK(AsyncGetStatus(&block, false) == kPending);
    size_t size = 0;
    CHECK(AsyncGetResultSize(&block, &size) == kPending);

    REQUIRE(TaskQueueDispatch(queue.Get(), Port::Work, 0)); // DoWork.
    CHECK(AsyncGetStatus(&block, false) == kOk);
    CHECK(completed.Count == 0); // The completion routine runs on the completion port.
    REQUIRE(TaskQueueDispatch(queue.Get(), Port::Completion, 0));
    CHECK(completed.Count == 1);
    CHECK(completed.Status == kOk);

    REQUIRE(AsyncGetResultSize(&block, &size) == kOk);
    CHECK(size == 4);
    uint32_t value = 0;
    size_t used = 0;
    CHECK(AsyncGetResult(&block, &kIdentity, 2, &value, &used) == kInsufficientBuffer);
    CHECK(AsyncGetResult(&block, nullptr, sizeof(value), &value, &used) == kInvalidArgument); // Wrong identity.
    CHECK(provider.Cleanups == 0);
    REQUIRE(AsyncGetResult(&block, &kIdentity, sizeof(value), &value, &used) == kOk);
    CHECK(value == 0xC0FFEE);
    CHECK(used == 4);
    CHECK(provider.Cleanups == 1);
    CHECK(provider.Ops == std::vector<AsyncOp>{AsyncOp::Begin, AsyncOp::DoWork, AsyncOp::GetResult, AsyncOp::Cleanup});

    // The block no longer belongs to the call, but remembers how it ended.
    CHECK(AsyncGetStatus(&block, false) == kOk);
    CHECK(AsyncGetResult(&block, &kIdentity, sizeof(value), &value, &used) == kNotSupported);
}

TEST_CASE("Canceling an XAsync call ends it with E_ABORT", "[gdk][xasync]")
{
    const Queue queue(Mode::Manual, Mode::Immediate);
    Provider provider;
    provider.CompleteOnWork = false;
    AsyncBlock block{};
    block.Queue = queue.Get();
    REQUIRE(AsyncBegin(&block, &provider, nullptr, nullptr, &Provider::Call) == kOk);
    REQUIRE(TaskQueueDispatch(queue.Get(), Port::Work, 0));
    CHECK(AsyncGetStatus(&block, false) == kPending);

    AsyncCancel(&block);
    CHECK(AsyncGetStatus(&block, false) == kAbort);
    size_t size = 0;
    CHECK(AsyncGetResultSize(&block, &size) == kAbort);
    CHECK(provider.Ops.at(2) == AsyncOp::Cancel);
    CHECK(provider.Cleanups == 1);
    AsyncCancel(&block); // Already finished: nothing happens.
    CHECK(provider.Cleanups == 1);
}

TEST_CASE("A failing Begin ends the call at once", "[gdk][xasync]")
{
    const Queue queue(Mode::Manual, Mode::Manual);
    struct Failing
    {
        int Cleanups = 0;
    } failing;
    const auto provider = [](AsyncOp op, const AsyncProviderData* data) noexcept -> int32_t
    {
        if (op == AsyncOp::Cleanup)
        {
            ++static_cast<Failing*>(data->Context)->Cleanups;
        }
        return op == AsyncOp::Begin ? kInvalidArgument : kOk;
    };
    AsyncBlock block{};
    block.Queue = queue.Get();
    CHECK(AsyncBegin(&block, &failing, nullptr, nullptr, provider) == kInvalidArgument);
    CHECK(failing.Cleanups == 1);
    CHECK_FALSE(TaskQueueDispatch(queue.Get(), Port::Work, 0));
}

TEST_CASE("XAsyncRun runs work on a thread pool queue and can be waited on", "[gdk][xasync]")
{
    const Queue queue(Mode::ThreadPool, Mode::ThreadPool);
    AsyncBlock block{};
    block.Queue = queue.Get();
    static std::atomic<DWORD> workThread{0};
    const auto work = [](AsyncBlock* /*async*/) noexcept -> int32_t
    {
        workThread = GetCurrentThreadId();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return std::bit_cast<int32_t>(0x8000'4005U);
    };
    REQUIRE(AsyncRun(&block, work) == kOk);
    CHECK(AsyncGetStatus(&block, true) == std::bit_cast<int32_t>(0x8000'4005U));
    CHECK(workThread != GetCurrentThreadId());
}

TEST_CASE("XGameRuntime and XSystem report the running title", "[gdk][runtime]")
{
    ShimRegistry shims;
    REQUIRE(RegisterGameRuntimeShims(shims));
    const auto shim = [&shims]<typename Function>(std::string_view name, Function* /*type*/)
    {
        const auto resolved = shims.Resolve("XGameRuntime.dll", name);
        REQUIRE(resolved.has_value());
        return FromShimProc<Function>(resolved->Export->Proc);
    };
    auto* const initialize = shim("XGameRuntimeInitialize", static_cast<int32_t (*)()>(nullptr));
    auto* const available = shim("XGameRuntimeIsFeatureAvailable", static_cast<bool (*)(uint32_t)>(nullptr));
    auto* const titleId = shim("XGameGetXboxTitleId", static_cast<int32_t (*)(uint32_t*)>(nullptr));
    auto* const device = shim("XSystemGetDeviceType", static_cast<uint32_t (*)()>(nullptr));
    auto* const consoleId = shim("XSystemGetConsoleId", static_cast<int32_t (*)(size_t, char*, size_t*)>(nullptr));
    auto* const sandbox =
        shim("XSystemGetXboxLiveSandboxId", static_cast<int32_t (*)(size_t, char*, size_t*)>(nullptr));
    CHECK(shims.Resolve("xgameruntime.dll", "XTaskQueueCreate").has_value());
    CHECK(shims.Resolve("xgameruntime.dll", "XAsyncRun").has_value());

    CHECK(initialize() == kOk);
    CHECK(available(16));       // XTaskQueue
    CHECK(available(2));        // XAsync
    CHECK_FALSE(available(18)); // XUser

    SetGameRuntimeTitle({.TitleId = std::nullopt, .Device = DeviceType::Unknown});
    uint32_t id = 0;
    CHECK(titleId(&id) == std::bit_cast<int32_t>(0x8007'0490U));
    SetGameRuntimeTitle({.TitleId = ParseTitleId("0x1A2B3C4D"), .Device = DeviceForFamily("scarlett")});
    CHECK(titleId(&id) == kOk);
    CHECK(id == 0x1A2B'3C4DU);
    CHECK(device() == 0x07);

    std::array<char, 64> text{};
    size_t used = 0;
    CHECK(consoleId(8, text.data(), &used) == kInsufficientBuffer);
    REQUIRE(consoleId(text.size(), text.data(), &used) == kOk);
    CHECK(std::string(text.data()) == "00000000.00000000.00000000.00000000.00");
    CHECK(used == 39);
    REQUIRE(sandbox(text.size(), text.data(), &used) == kOk);
    CHECK(std::string(text.data()) == "RETAIL");
    SetGameRuntimeTitle({});
}

TEST_CASE("Title ids and device families parse from MicrosoftGame.config", "[gdk][runtime]")
{
    CHECK(ParseTitleId("1A2B3C4D") == std::optional<uint32_t>(0x1A2B'3C4D));
    CHECK(ParseTitleId("0xff") == std::optional<uint32_t>(0xFF));
    CHECK_FALSE(ParseTitleId("").has_value());
    CHECK_FALSE(ParseTitleId("123456789").has_value());
    CHECK_FALSE(ParseTitleId("12G4").has_value());
    CHECK(DeviceForFamily("Scarlett") == DeviceType::XboxScarlettAnaconda);
    CHECK(DeviceForFamily("XboxOne") == DeviceType::XboxOne);
    CHECK(DeviceForFamily("PC") == DeviceType::Pc);
    CHECK(DeviceForFamily("Toaster") == DeviceType::Unknown);
}
