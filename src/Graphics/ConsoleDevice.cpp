#include "ConsoleDevice.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>

#include <Windows.h>
#include <unknwn.h>

#include "Kernel/ShimGuard.h"
#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"

namespace OpenEra::Graphics
{
namespace
{
constexpr std::string_view kModule = "d3d12_xs.dll";

// HRESULT is long and Kernel::HResult is int32_t: the same 32 bits.
HRESULT ToHresult(Kernel::HResult value) noexcept
{
    return std::bit_cast<HRESULT>(value);
}

Kernel::HResult FromHresult(HRESULT value) noexcept
{
    return std::bit_cast<Kernel::HResult>(value);
}

std::atomic<Kernel::StubRegistry*>& Stubs() noexcept
{
    static std::atomic<Kernel::StubRegistry*> stubs{nullptr};
    return stubs;
}

void Record(const std::string& symbol, std::string_view note) noexcept
{
    if (auto* const stubs = Stubs().load())
    {
        std::ignore = stubs->RecordHit(Kernel::StubKind::Win32Import, kModule, symbol, note);
    }
}

std::string FormatGuid(const GUID& id)
{
    const auto bytes = std::bit_cast<std::array<uint8_t, 8>>(id.Data4);
    return std::format("{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}", id.Data1, id.Data2,
                       id.Data3, bytes.at(0), bytes.at(1), bytes.at(2), bytes.at(3), bytes.at(4), bytes.at(5),
                       bytes.at(6), bytes.at(7));
}

struct RecordingObject;
// Every slot has this shape: `this` and up to three register arguments. Extra
// arguments are on the stack, which the caller cleans up on x64.
using SlotFunction = HRESULT(STDMETHODCALLTYPE*)(RecordingObject* self, uint64_t first, uint64_t second,
                                                 uint64_t third) noexcept;

// A COM object as callers see it: a vtable pointer first.
struct RecordingObject
{
    const SlotFunction* Vtable = nullptr;
    std::atomic<uint32_t> References{1};
};

HRESULT QueryInterface(RecordingObject* self, uint64_t riid, uint64_t object)
{
    auto* const out = std::bit_cast<void**>(object);
    const auto* const id = std::bit_cast<const GUID*>(riid);
    if (out == nullptr || id == nullptr)
    {
        return E_POINTER;
    }
    Record(std::format("device::QueryInterface {}", FormatGuid(*id)), "recording stand-in");
    self->References.fetch_add(1);
    *out = self;
    return S_OK;
}

// One shape for every slot, so `self` can't be const even where a slot only records.
template <size_t Slot>
[[gsl::suppress("con.3")]] HRESULT STDMETHODCALLTYPE Dispatch(RecordingObject* self, uint64_t first, uint64_t second,
                                                              uint64_t /*third*/) noexcept
{
    if (self == nullptr)
    {
        return E_POINTER;
    }
    if constexpr (Slot == 0)
    {
        return ToHresult(Kernel::GuardHResult("device::QueryInterface",
                                              [&] { return FromHresult(QueryInterface(self, first, second)); }));
    }
    else if constexpr (Slot == 1)
    {
        return std::bit_cast<HRESULT>(self->References.fetch_add(1) + 1);
    }
    else if constexpr (Slot == 2)
    {
        const uint32_t remaining = self->References.fetch_sub(1) - 1;
        if (remaining == 0)
        {
            std::unique_ptr<RecordingObject>{self}.reset();
        }
        return std::bit_cast<HRESULT>(remaining);
    }
    else
    {
        try
        {
            Record(std::format("device::slot{:03}", Slot), "recording stand-in");
        }
        catch (...) // NOLINT(bugprone-empty-catch): recording is best effort.
        {
        }
        return E_NOTIMPL;
    }
}

template <size_t... Slots>
constexpr std::array<SlotFunction, sizeof...(Slots)> MakeVtable(std::index_sequence<Slots...> /*slots*/) noexcept
{
    return {&Dispatch<Slots>...};
}

constexpr auto kVtable = MakeVtable(std::make_index_sequence<kRecordedSlots>{});

HRESULT WINAPI D3D12XboxCreateDeviceShim(IUnknown* /*adapter*/, const void* /*parameters*/, REFIID riid,
                                         void** device) noexcept
{
    return ToHresult(Kernel::GuardHResult("D3D12XboxCreateDevice",
                                          [&]() -> Kernel::HResult
                                          {
                                              if (device == nullptr)
                                              {
                                                  return FromHresult(E_POINTER);
                                              }
                                              *device = nullptr;
                                              Record(std::format("D3D12XboxCreateDevice {}", FormatGuid(riid)),
                                                     "returned a recording stand-in");
                                              auto object = std::make_unique<RecordingObject>();
                                              object->Vtable = kVtable.data();
                                              *device = object.release();
                                              return Kernel::kHResultOk;
                                          }));
}
} // namespace

bool AddConsoleDeviceExports(Kernel::ShimModule& module, Kernel::StubRegistry* stubs)
{
    Stubs().store(stubs);
    return module.Export("D3D12XboxCreateDevice", Kernel::ToShimProc(&D3D12XboxCreateDeviceShim));
}
} // namespace OpenEra::Graphics
