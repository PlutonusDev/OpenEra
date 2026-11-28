#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Kernel/TaskQueue.h"

namespace OpenEra::Kernel::Gdk
{
// The GDK's XAsync and XAsyncProvider API, written from its public documentation.
struct AsyncBlock;
using AsyncCompletionRoutine = void(AsyncBlock* asyncBlock);

#pragma pack(push, 8)
struct AsyncBlock
{
    TaskQueueHandle Queue;            // Null: the process queue.
    void* Context;                    // For the completion routine.
    AsyncCompletionRoutine* Callback; // Optional.
    std::array<unsigned char, sizeof(void*) * 4> Internal;
};
#pragma pack(pop)
static_assert(sizeof(AsyncBlock) == 56);

enum class AsyncOp : uint32_t // NOLINT(performance-enum-size): ABI type.
{
    Begin,
    DoWork,
    GetResult,
    Cancel,
    Cleanup,
};

#pragma pack(push, 8)
struct AsyncProviderData
{
    AsyncBlock* Async;
    size_t BufferSize; // GetResult only.
    void* Buffer;      // GetResult only.
    void* Context;     // From AsyncBegin.
};
#pragma pack(pop)
static_assert(sizeof(AsyncProviderData) == 32);

using AsyncProvider = int32_t(AsyncOp op, const AsyncProviderData* data);
using AsyncWork = int32_t(AsyncBlock* asyncBlock);

constexpr int32_t kPending = std::bit_cast<int32_t>(0x8000'000AU);
constexpr int32_t kNotSupported = std::bit_cast<int32_t>(0x8007'0032U); // HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)
constexpr int32_t kInsufficientBuffer =
    std::bit_cast<int32_t>(0x8007'007AU); // HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)

int32_t AsyncBegin(AsyncBlock* asyncBlock, void* context, const void* identity, const char* identityName,
                   AsyncProvider* provider) noexcept;
int32_t AsyncSchedule(AsyncBlock* asyncBlock, uint32_t delayInMs) noexcept;
void AsyncComplete(AsyncBlock* asyncBlock, int32_t result, size_t requiredBufferSize) noexcept;
int32_t AsyncGetStatus(AsyncBlock* asyncBlock, bool wait) noexcept;
int32_t AsyncGetResultSize(AsyncBlock* asyncBlock, size_t* bufferSize) noexcept;
int32_t AsyncGetResult(AsyncBlock* asyncBlock, const void* identity, size_t bufferSize, void* buffer,
                       size_t* bufferUsed) noexcept;
void AsyncCancel(AsyncBlock* asyncBlock) noexcept;
int32_t AsyncRun(AsyncBlock* asyncBlock, AsyncWork* work) noexcept;
} // namespace OpenEra::Kernel::Gdk
