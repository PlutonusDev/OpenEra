#pragma once

#include <cstddef>

namespace OpenEra::Kernel
{
class ShimModule;
class StubRegistry;
} // namespace OpenEra::Kernel

namespace OpenEra::Graphics
{
// The console Direct3D 12.x device (d3d12_xs.dll on Xbox Series X|S, d3d12_x.dll
// on Xbox One). Its interfaces aren't publicly documented, so this is the
// observation step of #27: D3D12XboxCreateDevice returns a recording stand-in.
// - QueryInterface, AddRef and Release behave as COM requires; QueryInterface
//   hands back the same stand-in and records each interface ID asked for.
// - Every other vtable slot records "device::slotNNN" in `stubs` and returns
//   E_NOTIMPL, so the unimplemented-call summary shows which methods titles call.
// The call shape (adapter, parameters, riid, device) follows Microsoft's public,
// MIT-licensed Xbox-GDK-Samples (SimpleTriangle, DeviceResources.cpp). The
// parameter block isn't read.
constexpr size_t kRecordedSlots = 256;

// Adds D3D12XboxCreateDevice to `module`. Hits go to `stubs` (may be null in tests).
bool AddConsoleDeviceExports(Kernel::ShimModule& module, Kernel::StubRegistry* stubs);
} // namespace OpenEra::Graphics
