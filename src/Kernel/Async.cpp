#include "Async.h"

#include <algorithm>
#include <array>
#include <bit>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace OpenEra::Kernel::Gdk
{
namespace
{
// A call in flight. Blocks map to their call while it's associated with one; once
// it no longer is, the block's Internal bytes keep the final status for later queries.
struct AsyncCall
{
    AsyncProvider* Provider = nullptr;
    void* Context = nullptr;
    const void* Identity = nullptr;
    std::string IdentityName;
    TaskQueueHandle Queue = nullptr; // A reference, released at cleanup.

    std::mutex Mutex;
    std::condition_variable Done;
    int32_t Status = kPending;
    size_t ResultSize = 0;
    uint32_t WorkInFlight = 0;
    bool Associated = true; // The result hasn't been taken (or there's none).
    bool CleanedUp = false;
};

// What a finished call leaves in AsyncBlock::Internal.
struct FinalState
{
    uint32_t Magic;
    int32_t Status;
    uint64_t Size;
};
constexpr uint32_t kFinalMagic = 0x4F45'4153; // "OEAS"

struct Calls
{
    std::mutex Mutex;
    std::unordered_map<const AsyncBlock*, std::shared_ptr<AsyncCall>> ByBlock;
};

Calls& Registry()
{
    static Calls calls;
    return calls;
}

std::shared_ptr<AsyncCall> Find(const AsyncBlock* block)
{
    auto& registry = Registry();
    const std::scoped_lock lock(registry.Mutex);
    const auto it = registry.ByBlock.find(block);
    return it == registry.ByBlock.end() ? nullptr : it->second;
}

void Forget(const AsyncBlock* block, const AsyncCall& call)
{
    auto& registry = Registry();
    const std::scoped_lock lock(registry.Mutex);
    const auto it = registry.ByBlock.find(block);
    if (it != registry.ByBlock.end() && it->second.get() == &call)
    {
        registry.ByBlock.erase(it);
    }
}

void WriteFinal(AsyncBlock& block, int32_t status, size_t size) noexcept
{
    const FinalState state{.Magic = kFinalMagic, .Status = status, .Size = size};
    const auto bytes = std::bit_cast<std::array<unsigned char, sizeof(FinalState)>>(state);
    std::ranges::copy(bytes, block.Internal.begin());
}

std::optional<FinalState> ReadFinal(const AsyncBlock& block) noexcept
{
    std::array<unsigned char, sizeof(FinalState)> bytes{};
    std::ranges::copy_n(block.Internal.begin(), bytes.size(), bytes.begin());
    const auto state = std::bit_cast<FinalState>(bytes);
    return state.Magic == kFinalMagic ? std::optional(state) : std::nullopt;
}

AsyncProviderData DataFor(AsyncBlock* block, const AsyncCall& call, size_t bufferSize = 0,
                          void* buffer = nullptr) noexcept
{
    return {.Async = block, .BufferSize = bufferSize, .Buffer = buffer, .Context = call.Context};
}

// The provider's Cleanup, once: after completion, once the result is taken (or
// there's none), and once no DoWork is running.
void CleanupIfDone(AsyncBlock* block, AsyncCall& call)
{
    {
        const std::scoped_lock lock(call.Mutex);
        if (call.CleanedUp || call.Status == kPending || call.Associated || call.WorkInFlight != 0)
        {
            return;
        }
        call.CleanedUp = true;
    }
    const auto data = DataFor(block, call);
    std::ignore = call.Provider(AsyncOp::Cleanup, &data);
    TaskQueueCloseHandle(std::exchange(call.Queue, nullptr));
}

void CompleteCall(AsyncBlock* block, AsyncCall& call, int32_t result, size_t size)
{
    if (block == nullptr)
    {
        return;
    }
    AsyncCompletionRoutine* callback = nullptr;
    bool associated = false;
    TaskQueueHandle queue = nullptr;
    {
        const std::scoped_lock lock(call.Mutex);
        if (call.Status != kPending)
        {
            return;
        }
        call.Status = result;
        call.ResultSize = result >= 0 ? size : 0;
        call.Associated = call.ResultSize != 0;
        associated = call.Associated;
        // Read before anyone can see the call finished: the block may be freed after that.
        callback = block->Callback;
        std::ignore = TaskQueueDuplicateHandle(call.Queue, &queue);
        WriteFinal(*block, call.Status, call.ResultSize);
    }
    if (!associated)
    {
        Forget(block, call);
    }
    call.Done.notify_all();
    CleanupIfDone(block, call);

    if (callback != nullptr)
    {
        struct Completion
        {
            AsyncCompletionRoutine* Callback;
            AsyncBlock* Block;
        };
        auto completion = std::make_unique<Completion>(Completion{.Callback = callback, .Block = block});
        const auto invoke = [](void* context, bool /*canceled*/) noexcept
        {
            const std::unique_ptr<Completion> owned(static_cast<Completion*>(context));
            if (owned)
            {
                owned->Callback(owned->Block); // Runs even for a terminated queue.
            }
        };
        if (TaskQueueSubmitCallback(queue, TaskQueuePort::Completion, completion.get(), invoke) == kOk)
        {
            std::ignore = completion.release();
        }
        else
        {
            callback(block);
        }
    }
    TaskQueueCloseHandle(queue);
}

int32_t Guarded(auto&& function) noexcept
{
    try
    {
        return function();
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

struct WorkItem
{
    AsyncBlock* Block;
    std::shared_ptr<AsyncCall> Call;
};

void RunWork(void* context, bool canceled) noexcept
{
    const std::unique_ptr<WorkItem> item(static_cast<WorkItem*>(context));
    if (!item)
    {
        return;
    }
    std::ignore = Guarded(
        [&]
        {
            auto& call = *item->Call;
            bool pending = false;
            {
                const std::scoped_lock lock(call.Mutex);
                pending = call.Status == kPending;
            }
            if (canceled)
            {
                CompleteCall(item->Block, call, kAbort, 0);
            }
            else if (pending)
            {
                const auto data = DataFor(item->Block, call);
                const int32_t result = call.Provider(AsyncOp::DoWork, &data);
                if (result != kPending)
                {
                    CompleteCall(item->Block, call, result, 0); // Ignored if the provider completed it.
                }
            }
            {
                const std::scoped_lock lock(call.Mutex);
                --call.WorkInFlight;
            }
            CleanupIfDone(item->Block, call);
            return kOk;
        });
}

int32_t RunProvider(AsyncOp op, const AsyncProviderData* data) noexcept
{
    if (data == nullptr)
    {
        return kInvalidArgument;
    }
    switch (op)
    {
    case AsyncOp::Begin:
        return AsyncSchedule(data->Async, 0);
    case AsyncOp::DoWork:
        return std::bit_cast<AsyncWork*>(std::bit_cast<uintptr_t>(data->Context))(data->Async);
    case AsyncOp::GetResult:
    case AsyncOp::Cancel:
    case AsyncOp::Cleanup:
        break;
    }
    return kOk;
}
} // namespace

int32_t AsyncBegin(AsyncBlock* asyncBlock, void* context, const void* identity, const char* identityName,
                   AsyncProvider* provider) noexcept
{
    if (asyncBlock == nullptr || provider == nullptr)
    {
        return kInvalidArgument;
    }
    return Guarded(
        [&]
        {
            TaskQueueHandle queue = TaskQueueOrProcess(asyncBlock->Queue);
            if (queue == nullptr)
            {
                return kInvalidArgument; // No queue and no process queue.
            }
            auto call = std::make_shared<AsyncCall>();
            call->Provider = provider;
            call->Context = context;
            call->Identity = identity;
            call->IdentityName = identityName == nullptr ? "" : identityName;
            call->Queue = queue;
            asyncBlock->Internal.fill(0);
            {
                auto& registry = Registry();
                const std::scoped_lock lock(registry.Mutex);
                registry.ByBlock.insert_or_assign(asyncBlock, call);
            }
            const auto data = DataFor(asyncBlock, *call);
            const int32_t result = provider(AsyncOp::Begin, &data);
            if (result < 0)
            {
                Forget(asyncBlock, *call);
                {
                    const std::scoped_lock lock(call->Mutex);
                    call->Status = result;
                    call->Associated = false;
                    call->CleanedUp = true;
                }
                std::ignore = provider(AsyncOp::Cleanup, &data);
                TaskQueueCloseHandle(std::exchange(call->Queue, nullptr));
                return result;
            }
            return kOk;
        });
}

int32_t AsyncSchedule(AsyncBlock* asyncBlock, uint32_t delayInMs) noexcept
{
    return Guarded(
        [&]
        {
            auto call = Find(asyncBlock);
            if (!call)
            {
                return kInvalidArgument;
            }
            TaskQueueHandle queue = nullptr;
            {
                const std::scoped_lock lock(call->Mutex);
                if (call->Status != kPending)
                {
                    return kAbort;
                }
                ++call->WorkInFlight;
                queue = call->Queue;
            }
            auto item = std::make_unique<WorkItem>(WorkItem{.Block = asyncBlock, .Call = call});
            const int32_t result =
                TaskQueueSubmitDelayedCallback(queue, TaskQueuePort::Work, delayInMs, item.get(), &RunWork);
            if (result == kOk)
            {
                std::ignore = item.release();
                return kOk;
            }
            {
                const std::scoped_lock lock(call->Mutex);
                --call->WorkInFlight;
            }
            return result;
        });
}

void AsyncComplete(AsyncBlock* asyncBlock, int32_t result, size_t requiredBufferSize) noexcept
{
    if (result == kPending)
    {
        return; // More work is coming.
    }
    std::ignore = Guarded(
        [&]
        {
            if (auto call = Find(asyncBlock))
            {
                CompleteCall(asyncBlock, *call, result, requiredBufferSize);
            }
            return kOk;
        });
}

int32_t AsyncGetStatus(AsyncBlock* asyncBlock, bool wait) noexcept
{
    if (asyncBlock == nullptr)
    {
        return kInvalidArgument;
    }
    return Guarded(
        [&]
        {
            if (auto call = Find(asyncBlock))
            {
                std::unique_lock lock(call->Mutex);
                if (wait)
                {
                    call->Done.wait(lock, [&call]() noexcept { return call->Status != kPending; });
                }
                return call->Status;
            }
            const auto final = ReadFinal(*asyncBlock);
            return final ? final->Status : kInvalidArgument;
        });
}

int32_t AsyncGetResultSize(AsyncBlock* asyncBlock, size_t* bufferSize) noexcept
{
    if (asyncBlock == nullptr || bufferSize == nullptr)
    {
        return kInvalidArgument;
    }
    return Guarded(
        [&]
        {
            if (auto call = Find(asyncBlock))
            {
                const std::scoped_lock lock(call->Mutex);
                if (call->Status < 0)
                {
                    return call->Status;
                }
                *bufferSize = call->ResultSize;
                return kOk;
            }
            const auto final = ReadFinal(*asyncBlock);
            if (!final)
            {
                return kInvalidArgument;
            }
            if (final->Status < 0)
            {
                return final->Status;
            }
            *bufferSize = 0; // Taken, or there was none.
            return kOk;
        });
}

int32_t AsyncGetResult(AsyncBlock* asyncBlock, const void* identity, size_t bufferSize, void* buffer,
                       size_t* bufferUsed) noexcept
{
    if (asyncBlock == nullptr)
    {
        return kInvalidArgument;
    }
    return Guarded(
        [&]
        {
            auto call = Find(asyncBlock);
            if (!call)
            {
                const auto final = ReadFinal(*asyncBlock);
                if (!final)
                {
                    return kInvalidArgument;
                }
                return final->Status < 0 ? final->Status : kNotSupported;
            }
            size_t size = 0;
            {
                const std::scoped_lock lock(call->Mutex);
                if (call->Status < 0) // Failed, or still pending.
                {
                    return call->Status;
                }
                if (call->Identity != identity)
                {
                    return kInvalidArgument;
                }
                size = call->ResultSize;
            }
            if (buffer == nullptr || bufferSize < size)
            {
                return kInsufficientBuffer;
            }
            const auto data = DataFor(asyncBlock, *call, bufferSize, buffer);
            const int32_t result = call->Provider(AsyncOp::GetResult, &data);
            if (bufferUsed != nullptr)
            {
                *bufferUsed = size;
            }
            {
                const std::scoped_lock lock(call->Mutex);
                call->Associated = false;
            }
            Forget(asyncBlock, *call);
            CleanupIfDone(asyncBlock, *call);
            return result;
        });
}

void AsyncCancel(AsyncBlock* asyncBlock) noexcept
{
    std::ignore = Guarded(
        [&]
        {
            auto call = Find(asyncBlock);
            if (!call)
            {
                return kOk;
            }
            {
                const std::scoped_lock lock(call->Mutex);
                if (call->Status != kPending)
                {
                    return kOk;
                }
            }
            const auto data = DataFor(asyncBlock, *call);
            std::ignore = call->Provider(AsyncOp::Cancel, &data);
            CompleteCall(asyncBlock, *call, kAbort, 0); // Unless the provider completed it already.
            return kOk;
        });
}

int32_t AsyncRun(AsyncBlock* asyncBlock, AsyncWork* work) noexcept
{
    if (work == nullptr)
    {
        return kInvalidArgument;
    }
    return AsyncBegin(asyncBlock, std::bit_cast<void*>(std::bit_cast<uintptr_t>(work)), nullptr, "XAsyncRun",
                      &RunProvider);
}
} // namespace OpenEra::Kernel::Gdk
