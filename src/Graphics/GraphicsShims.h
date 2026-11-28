#pragma once

namespace OpenEra::Kernel
{
class ShimRegistry;
class StubRegistry;
} // namespace OpenEra::Kernel

namespace OpenEra::Graphics
{
// Registers desktop graphics shims that put the preferred GPU (SelectAdapter
// with CurrentGpuPreference) in front of titles, on NVIDIA and AMD alike:
// - dxgi.dll: CreateDXGIFactory, CreateDXGIFactory1 and CreateDXGIFactory2
//   return factories that enumerate the preferred adapter first.
// - d3d12.dll: D3D12CreateDevice with no adapter uses the preferred one.
// - d3d12_xs.dll / d3d12_x.dll: D3D12XboxCreateDevice returns a recording
//   stand-in device (ConsoleDevice.h) whose calls are recorded in `stubs`.
// Other exports of these modules pass through to the host's own DLLs.
bool RegisterGraphicsShims(Kernel::ShimRegistry& shims, Kernel::StubRegistry* stubs = nullptr);
} // namespace OpenEra::Graphics
