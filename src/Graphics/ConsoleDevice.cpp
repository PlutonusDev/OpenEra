#include "ConsoleDevice.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "Graphics/DxgiFactory.h"
#include "Kernel/ShimGuard.h"
#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"

namespace OpenEra::Graphics
{
namespace
{
using Microsoft::WRL::ComPtr;

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

void Record(std::string_view symbol, std::string_view note) noexcept
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

// The kinds of console object OpenEra hands out. Each has its own vtable.
enum class Kind : uint8_t
{
    Device,
    Queue,
    Allocator,
    List,
    Fence,
    Heap,
};

constexpr std::array<std::string_view, 6> kKindNames = {"device", "queue", "allocator", "list", "fence", "heap"};

constexpr std::string_view NameOf(Kind kind) noexcept
{
    return kKindNames.at(static_cast<size_t>(kind));
}

struct StandIn;
// Every recorded slot has this shape: `this`, three register arguments and the
// first two stack arguments. On x64 the caller reserves and cleans up the stack,
// so reading two stack slots is safe even when a method takes fewer arguments.
using SlotFunction = HRESULT(STDMETHODCALLTYPE*)(StandIn* self, uint64_t first, uint64_t second, uint64_t third,
                                                 uint64_t fourth, uint64_t fifth) noexcept;

// A COM object as callers see it: a vtable pointer first. `Inner` is the desktop
// object it stands for, or null when there's none.
struct StandIn
{
    const SlotFunction* Vtable = nullptr;
    std::atomic<uint32_t> References{1};
    Kind Type = Kind::Device;
    ComPtr<IUnknown> Inner;
};

const SlotFunction* VtableFor(Kind kind) noexcept;

StandIn* Wrap(Kind kind, ComPtr<IUnknown> inner)
{
    auto object = std::make_unique<StandIn>();
    object->Vtable = VtableFor(kind);
    object->Type = kind;
    object->Inner = std::move(inner);
    return object.release();
}

bool IsStandIn(const void* object) noexcept;

// A register argument for the log: its value, and which stand-in it is if it's one of ours.
std::string Describe(uint64_t value);

template <typename Interface>
Interface* InnerAs(const StandIn* self) noexcept
{
    return self == nullptr ? nullptr : static_cast<Interface*>(self->Inner.Get());
}

HRESULT QueryInterface(StandIn* self, uint64_t riid, uint64_t object)
{
    auto* const out = std::bit_cast<void**>(object);
    const auto* const id = std::bit_cast<const GUID*>(riid);
    if (out == nullptr || id == nullptr)
    {
        return E_POINTER;
    }
    Record(std::format("{}::QueryInterface {}", NameOf(self->Type), FormatGuid(*id)), "recording stand-in");
    self->References.fetch_add(1);
    *out = self;
    return S_OK;
}

// Unconfirmed slots record themselves; 0-2 are IUnknown.
template <size_t Slot>
[[gsl::suppress("con.3")]] HRESULT STDMETHODCALLTYPE Dispatch(StandIn* self, uint64_t first, uint64_t second,
                                                              uint64_t third, uint64_t fourth, uint64_t fifth) noexcept
{
    if (self == nullptr)
    {
        return E_POINTER;
    }
    if constexpr (Slot == 0)
    {
        return ToHresult(Kernel::GuardHResult("stand-in QueryInterface",
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
            std::unique_ptr<StandIn>{self}.reset();
        }
        return std::bit_cast<HRESULT>(remaining);
    }
    else
    {
        try
        {
            // The first hit is logged with its register arguments, which is what matches a slot to a method.
            Record(std::format("{}::slot{:03}", NameOf(self->Type), Slot),
                   std::format("recording stand-in; arguments {}, {}, {}, {}, {}", Describe(first), Describe(second),
                               Describe(third), Describe(fourth), Describe(fifth)));
        }
        catch (...) // NOLINT(bugprone-empty-catch): recording is best effort.
        {
        }
        return E_NOTIMPL;
    }
}

template <size_t... Slots>
constexpr std::array<SlotFunction, sizeof...(Slots)> MakeRecordingVtable(
    std::index_sequence<Slots...> /*slots*/) noexcept
{
    return {&Dispatch<Slots>...};
}

using Vtable = std::array<SlotFunction, kRecordedSlots>;

// Creates `Desktop` through `create` and returns it to the title as a `kind` stand-in.
template <typename Desktop, typename Create>
HRESULT CreateChild(Kind kind, REFIID riid, void** out, std::string_view method, Create create) noexcept
{
    return ToHresult(Kernel::GuardHResult(method,
                                          [&]() -> Kernel::HResult
                                          {
                                              if (out == nullptr)
                                              {
                                                  return FromHresult(E_POINTER);
                                              }
                                              *out = nullptr;
                                              if (riid != __uuidof(Desktop))
                                              {
                                                  Record(std::format("{} {}", method, FormatGuid(riid)),
                                                         "asked for a console-only interface ID");
                                              }
                                              ComPtr<Desktop> desktop;
                                              const HRESULT result = create(desktop);
                                              if (FAILED(result))
                                              {
                                                  return FromHresult(result);
                                              }
                                              *out = Wrap(kind, std::move(desktop));
                                              return Kernel::kHResultOk;
                                          }));
}

// ---- Confirmed slots ---------------------------------------------------------
// Each forwards to the desktop method in the same slot, and is listed with the
// observation that confirmed it (CONTRIBUTING: observing undocumented interfaces).

// device slot 8: CreateCommandQueue. A known first call after device creation landed here.
HRESULT STDMETHODCALLTYPE DeviceCreateCommandQueue(const StandIn* self, const D3D12_COMMAND_QUEUE_DESC* desc,
                                                   REFIID riid, void** queue) noexcept
{
    auto* const device = InnerAs<ID3D12Device>(self);
    if (device == nullptr)
    {
        Record("device::slot008", "no desktop device");
        return E_NOTIMPL;
    }
    return CreateChild<ID3D12CommandQueue>(Kind::Queue, riid, queue, "device::CreateCommandQueue",
                                           [&](ComPtr<ID3D12CommandQueue>& created) noexcept
                                           { return device->CreateCommandQueue(desc, IID_PPV_ARGS(&created)); });
}

// device slot 9: CreateCommandAllocator. The known next call (after the queue) landed here.
HRESULT STDMETHODCALLTYPE DeviceCreateCommandAllocator(const StandIn* self, D3D12_COMMAND_LIST_TYPE type, REFIID riid,
                                                       void** allocator) noexcept
{
    auto* const device = InnerAs<ID3D12Device>(self);
    if (device == nullptr)
    {
        Record("device::slot009", "no desktop device");
        return E_NOTIMPL;
    }
    return CreateChild<ID3D12CommandAllocator>(
        Kind::Allocator, riid, allocator, "device::CreateCommandAllocator",
        [&](ComPtr<ID3D12CommandAllocator>& created) noexcept
        { return device->CreateCommandAllocator(type, IID_PPV_ARGS(&created)); });
}

// The desktop object behind an argument the title passes back, if it's one of our stand-ins.
// Each stand-in's Inner was stored from that exact interface, so the downcast is exact.
template <typename Interface>
[[gsl::suppress("type.2")]] Interface* Unwrap(void* object) noexcept
{
    if (object == nullptr || !IsStandIn(object))
    {
        return static_cast<Interface*>(object);
    }
    return static_cast<Interface*>(static_cast<StandIn*>(object)->Inner.Get());
}

// device slot 73: the console's CreateCommandList. Observed at the known call (after the
// allocators) as (structure, allocator stand-in, null initial state, the desktop
// ID3D12GraphicsCommandList IID, out pointer), with the structure's first 16 bytes all zero
// for a direct list on node 0. Only that observed case is forwarded; any other structure is
// recorded with its bytes until its layout has been observed too.
HRESULT STDMETHODCALLTYPE DeviceCreateCommandListConsole(const StandIn* self, const std::array<uint64_t, 2>* parameters,
                                                         void* allocator, void* initialState, REFIID riid,
                                                         void** list) noexcept
{
    auto* const device = InnerAs<ID3D12Device>(self);
    if (device == nullptr || parameters == nullptr)
    {
        Record("device::slot073", "no desktop device, or no parameters");
        return E_NOTIMPL;
    }
    if (parameters->at(0) != 0 || parameters->at(1) != 0)
    {
        try
        {
            Record(std::format("device::slot073 parameters {:016x}{:016x}", parameters->at(0), parameters->at(1)),
                   "command list parameters not observed yet");
        }
        catch (...) // NOLINT(bugprone-empty-catch): recording is best effort.
        {
        }
        return E_NOTIMPL;
    }
    return CreateChild<ID3D12GraphicsCommandList>(
        Kind::List, riid, list, "device::CreateCommandList",
        [&](ComPtr<ID3D12GraphicsCommandList>& created) noexcept
        {
            return device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             Unwrap<ID3D12CommandAllocator>(allocator),
                                             Unwrap<ID3D12PipelineState>(initialState), IID_PPV_ARGS(&created));
        });
}

// device slot 36: CreateFence. Observed at the known call as (0, D3D12_FENCE_FLAG_NONE, the desktop
// ID3D12Fence IID, out pointer), the same as the desktop slot.
HRESULT STDMETHODCALLTYPE DeviceCreateFence(const StandIn* self, UINT64 initialValue, D3D12_FENCE_FLAGS flags,
                                            REFIID riid, void** fence) noexcept
{
    auto* const device = InnerAs<ID3D12Device>(self);
    if (device == nullptr)
    {
        Record("device::slot036", "no desktop device");
        return E_NOTIMPL;
    }
    return CreateChild<ID3D12Fence>(Kind::Fence, riid, fence, "device::CreateFence",
                                    [&](ComPtr<ID3D12Fence>& created) noexcept
                                    { return device->CreateFence(initialValue, flags, IID_PPV_ARGS(&created)); });
}

// list slot 9: Close. Observed as the known call right after the list was created, the same as
// the desktop slot.
// COM methods report failure through HRESULTs and never throw.
[[gsl::suppress("f.6")]] HRESULT STDMETHODCALLTYPE ListClose(const StandIn* self) noexcept
{
    auto* const list = InnerAs<ID3D12GraphicsCommandList>(self);
    return list == nullptr ? E_NOTIMPL : list->Close();
}

// device slot 14: CreateDescriptorHeap. Observed at the known call as (description, the desktop
// ID3D12DescriptorHeap IID, out pointer), the description reading Type 2 (RTV), 2 descriptors,
// no flags: the desktop slot, with the desktop D3D12_DESCRIPTOR_HEAP_DESC layout.
HRESULT STDMETHODCALLTYPE DeviceCreateDescriptorHeap(const StandIn* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                                     REFIID riid, void** heap) noexcept
{
    auto* const device = InnerAs<ID3D12Device>(self);
    if (device == nullptr)
    {
        Record("device::slot014", "no desktop device");
        return E_NOTIMPL;
    }
    return CreateChild<ID3D12DescriptorHeap>(Kind::Heap, riid, heap, "device::CreateDescriptorHeap",
                                             [&](ComPtr<ID3D12DescriptorHeap>& created) noexcept
                                             { return device->CreateDescriptorHeap(desc, IID_PPV_ARGS(&created)); });
}

// device slot 15: GetDescriptorHandleIncrementSize. Observed at the known call with argument 2
// (D3D12_DESCRIPTOR_HEAP_TYPE_RTV), the same as the desktop slot. Returns a UINT, not an HRESULT.
[[gsl::suppress("f.6")]] UINT STDMETHODCALLTYPE
DeviceGetDescriptorHandleIncrementSize(const StandIn* self, D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept
{
    auto* const device = InnerAs<ID3D12Device>(self);
    return device == nullptr ? 0 : device->GetDescriptorHandleIncrementSize(type);
}

template <typename Function>
[[gsl::suppress("type.1")]] SlotFunction AsSlot(Function* function) noexcept
{
    // Through void(*)(), the type-erased function pointer every compiler accepts a cast from.
    return reinterpret_cast<SlotFunction>(Kernel::ToShimProc(function));
}

Vtable MakeVtable(Kind kind) noexcept
{
    Vtable table = MakeRecordingVtable(std::make_index_sequence<kRecordedSlots>{});
    if (kind == Kind::Device)
    {
        table.at(8) = AsSlot(&DeviceCreateCommandQueue);
        table.at(9) = AsSlot(&DeviceCreateCommandAllocator);
        table.at(14) = AsSlot(&DeviceCreateDescriptorHeap);
        table.at(15) = AsSlot(&DeviceGetDescriptorHandleIncrementSize);
        table.at(36) = AsSlot(&DeviceCreateFence);
        table.at(73) = AsSlot(&DeviceCreateCommandListConsole);
    }
    else if (kind == Kind::List)
    {
        table.at(9) = AsSlot(&ListClose);
    }
    return table;
}

bool IsStandIn(const void* object) noexcept
{
    if (object == nullptr)
    {
        return false;
    }
    // Only objects whose first word is one of our vtables: compare the address, never dereference unknown memory twice.
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(object, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
    {
        return false;
    }
    const auto* const vtable = *static_cast<const SlotFunction* const*>(object);
    constexpr std::array kKinds = {Kind::Device, Kind::Queue, Kind::Allocator, Kind::List, Kind::Fence, Kind::Heap};
    return std::ranges::any_of(kKinds, [vtable](Kind kind) noexcept { return vtable == VtableFor(kind); });
}

std::string Describe(uint64_t value)
{
    const auto* const object = std::bit_cast<const void*>(value);
    if (!IsStandIn(object))
    {
        // For pointers to readable memory, the first 16 bytes: an interface ID, or a structure's start.
        MEMORY_BASIC_INFORMATION info{};
        if (value > 0xFFFF && VirtualQuery(object, &info, sizeof(info)) != 0 && info.State == MEM_COMMIT &&
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0 &&
            std::bit_cast<uint64_t>(info.BaseAddress) + info.RegionSize - value >= sizeof(GUID))
        {
            const GUID peek = *static_cast<const GUID*>(object);
            return std::format("{:#x} -> {}", value, FormatGuid(peek));
        }
        return std::format("{:#x}", value);
    }
    return std::format("{:#x} ({} stand-in)", value, NameOf(static_cast<const StandIn*>(object)->Type));
}

const SlotFunction* VtableFor(Kind kind) noexcept
{
    static const std::array<Vtable, 6> kTables = {MakeVtable(Kind::Device),    MakeVtable(Kind::Queue),
                                                  MakeVtable(Kind::Allocator), MakeVtable(Kind::List),
                                                  MakeVtable(Kind::Fence),     MakeVtable(Kind::Heap)};
    return kTables.at(static_cast<size_t>(kind)).data();
}

// A desktop device on the preferred GPU, or null if there's none.
ComPtr<ID3D12Device> CreateDesktopDevice() noexcept
{
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        try
        {
            adapter = PreferredAdapter(*factory.Get());
        }
        catch (...) // NOLINT(bugprone-empty-catch): fall back to the default adapter.
        {
        }
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
    {
        return nullptr;
    }
    return device;
}

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
                                              auto desktop = CreateDesktopDevice();
                                              Record(std::format("D3D12XboxCreateDevice {}", FormatGuid(riid)),
                                                     desktop ? "stand-in over a desktop device"
                                                             : "stand-in without a desktop device");
                                              *device = Wrap(Kind::Device, std::move(desktop));
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
