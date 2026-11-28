#include "GameRuntimeShims.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

#include "Kernel/Async.h"
#include "Kernel/ShimModule.h"
#include "Kernel/TaskQueue.h"

namespace OpenEra::Kernel
{
namespace
{
using Gdk::kInvalidArgument;
using Gdk::kOk;

constexpr int32_t kNotFound = std::bit_cast<int32_t>(0x8007'0490U); // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
// What the documentation says PCs and retail consoles report.
constexpr std::string_view kConsoleId = "00000000.00000000.00000000.00000000.00";
constexpr std::string_view kSandboxId = "RETAIL";

// XGameRuntimeFeature values the shims implement.
constexpr uint32_t kFeatureXAsync = 2;
constexpr uint32_t kFeatureXAsyncProvider = 3;
constexpr uint32_t kFeatureXGame = 5;
constexpr uint32_t kFeatureXSystem = 15;
constexpr uint32_t kFeatureXTaskQueue = 16;

struct TitleState
{
    std::mutex Mutex;
    GameRuntimeTitle Title;
};

TitleState& State() noexcept
{
    static TitleState state;
    return state;
}

GameRuntimeTitle CurrentTitle() noexcept
{
    auto& state = State();
    const std::scoped_lock lock(state.Mutex);
    return state.Title;
}

std::atomic<uint32_t>& Initialized() noexcept
{
    static std::atomic<uint32_t> count{0};
    return count;
}

// Copies `text` and its terminator; the buffer must hold both.
int32_t CopyId(std::string_view text, size_t size, char* buffer, size_t* used) noexcept
{
    if (buffer == nullptr)
    {
        return kInvalidArgument;
    }
    if (size < text.size() + 1)
    {
        return Gdk::kInsufficientBuffer;
    }
    const std::span<char> out(buffer, size);
    std::ranges::copy(text, out.begin());
    out.subspan(text.size()).front() = '\0';
    if (used != nullptr)
    {
        *used = text.size() + 1;
    }
    return kOk;
}

int32_t XGameRuntimeInitializeShim() noexcept
{
    Initialized().fetch_add(1);
    return kOk;
}

void XGameRuntimeUninitializeShim() noexcept
{
    uint32_t count = Initialized().load();
    while (count != 0 && !Initialized().compare_exchange_weak(count, count - 1))
    {
    }
}

bool XGameRuntimeIsFeatureAvailableShim(uint32_t feature) noexcept
{
    switch (feature)
    {
    case kFeatureXAsync:
    case kFeatureXAsyncProvider:
    case kFeatureXGame:
    case kFeatureXSystem:
    case kFeatureXTaskQueue:
        return true;
    default:
        return false;
    }
}

int32_t XGameGetXboxTitleIdShim(uint32_t* titleId) noexcept
{
    if (titleId == nullptr)
    {
        return kInvalidArgument;
    }
    const auto id = CurrentTitle().TitleId;
    if (!id)
    {
        return kNotFound;
    }
    *titleId = *id;
    return kOk;
}

uint32_t XSystemGetDeviceTypeShim() noexcept
{
    return std::bit_cast<uint32_t>(CurrentTitle().Device);
}

int32_t XSystemGetConsoleIdShim(size_t size, char* consoleId, size_t* used) noexcept
{
    return CopyId(kConsoleId, size, consoleId, used);
}

int32_t XSystemGetXboxLiveSandboxIdShim(size_t size, char* sandboxId, size_t* used) noexcept
{
    return CopyId(kSandboxId, size, sandboxId, used);
}
} // namespace

void SetGameRuntimeTitle(const GameRuntimeTitle& title) noexcept
{
    auto& state = State();
    const std::scoped_lock lock(state.Mutex);
    state.Title = title;
}

std::optional<uint32_t> ParseTitleId(std::string_view text) noexcept
{
    if (text.starts_with("0x") || text.starts_with("0X"))
    {
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 8)
    {
        return std::nullopt;
    }
    uint32_t value = 0;
    for (const char digit : text)
    {
        uint32_t nibble = 0;
        if (digit >= '0' && digit <= '9')
        {
            nibble = std::bit_cast<unsigned char>(digit) - uint32_t{'0'};
        }
        else if (digit >= 'a' && digit <= 'f')
        {
            nibble = std::bit_cast<unsigned char>(digit) - uint32_t{'a'} + 10;
        }
        else if (digit >= 'A' && digit <= 'F')
        {
            nibble = std::bit_cast<unsigned char>(digit) - uint32_t{'A'} + 10;
        }
        else
        {
            return std::nullopt;
        }
        value = (value << 4) | nibble;
    }
    return value;
}

DeviceType DeviceForFamily(std::string_view family) noexcept
{
    const auto equals = [family](std::string_view name) noexcept
    {
        return std::ranges::equal(family, name,
                                  [](char left, char right) noexcept { return (left | 0x20) == (right | 0x20); });
    };
    if (equals("Scarlett"))
    {
        return DeviceType::XboxScarlettAnaconda;
    }
    if (equals("XboxOne"))
    {
        return DeviceType::XboxOne;
    }
    if (equals("PC"))
    {
        return DeviceType::Pc;
    }
    return DeviceType::Unknown;
}

bool RegisterGameRuntimeShims(ShimRegistry& shims)
{
    ShimModule runtime("xgameruntime.dll", StubKind::GameCoreApi);
    const bool exported =
        runtime.Export("XGameRuntimeInitialize", ToShimProc(&XGameRuntimeInitializeShim)) &&
        runtime.Export("XGameRuntimeUninitialize", ToShimProc(&XGameRuntimeUninitializeShim)) &&
        runtime.Export("XGameRuntimeIsFeatureAvailable", ToShimProc(&XGameRuntimeIsFeatureAvailableShim)) &&
        runtime.Export("XGameGetXboxTitleId", ToShimProc(&XGameGetXboxTitleIdShim)) &&
        runtime.Export("XSystemGetDeviceType", ToShimProc(&XSystemGetDeviceTypeShim)) &&
        runtime.Export("XSystemGetConsoleId", ToShimProc(&XSystemGetConsoleIdShim)) &&
        runtime.Export("XSystemGetXboxLiveSandboxId", ToShimProc(&XSystemGetXboxLiveSandboxIdShim)) &&
        runtime.Export("XTaskQueueCreate", ToShimProc(&Gdk::TaskQueueCreate)) &&
        runtime.Export("XTaskQueueCreateComposite", ToShimProc(&Gdk::TaskQueueCreateComposite)) &&
        runtime.Export("XTaskQueueGetPort", ToShimProc(&Gdk::TaskQueueGetPort)) &&
        runtime.Export("XTaskQueueDuplicateHandle", ToShimProc(&Gdk::TaskQueueDuplicateHandle)) &&
        runtime.Export("XTaskQueueDispatch", ToShimProc(&Gdk::TaskQueueDispatch)) &&
        runtime.Export("XTaskQueueCloseHandle", ToShimProc(&Gdk::TaskQueueCloseHandle)) &&
        runtime.Export("XTaskQueueTerminate", ToShimProc(&Gdk::TaskQueueTerminate)) &&
        runtime.Export("XTaskQueueSubmitCallback", ToShimProc(&Gdk::TaskQueueSubmitCallback)) &&
        runtime.Export("XTaskQueueSubmitDelayedCallback", ToShimProc(&Gdk::TaskQueueSubmitDelayedCallback)) &&
        runtime.Export("XTaskQueueRegisterWaiter", ToShimProc(&Gdk::TaskQueueRegisterWaiter)) &&
        runtime.Export("XTaskQueueUnregisterWaiter", ToShimProc(&Gdk::TaskQueueUnregisterWaiter)) &&
        runtime.Export("XTaskQueueRegisterMonitor", ToShimProc(&Gdk::TaskQueueRegisterMonitor)) &&
        runtime.Export("XTaskQueueUnregisterMonitor", ToShimProc(&Gdk::TaskQueueUnregisterMonitor)) &&
        runtime.Export("XTaskQueueGetCurrentProcessTaskQueue", ToShimProc(&Gdk::TaskQueueGetCurrentProcessTaskQueue)) &&
        runtime.Export("XTaskQueueSetCurrentProcessTaskQueue", ToShimProc(&Gdk::TaskQueueSetCurrentProcessTaskQueue)) &&
        runtime.Export("XAsyncBegin", ToShimProc(&Gdk::AsyncBegin)) &&
        runtime.Export("XAsyncSchedule", ToShimProc(&Gdk::AsyncSchedule)) &&
        runtime.Export("XAsyncComplete", ToShimProc(&Gdk::AsyncComplete)) &&
        runtime.Export("XAsyncGetStatus", ToShimProc(&Gdk::AsyncGetStatus)) &&
        runtime.Export("XAsyncGetResultSize", ToShimProc(&Gdk::AsyncGetResultSize)) &&
        runtime.Export("XAsyncGetResult", ToShimProc(&Gdk::AsyncGetResult)) &&
        runtime.Export("XAsyncCancel", ToShimProc(&Gdk::AsyncCancel)) &&
        runtime.Export("XAsyncRun", ToShimProc(&Gdk::AsyncRun));
    return exported && shims.Register(std::move(runtime));
}
} // namespace OpenEra::Kernel
