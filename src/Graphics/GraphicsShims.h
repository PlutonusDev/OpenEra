#pragma once

namespace OpenEra::Kernel
{
class ShimRegistry;
} // namespace OpenEra::Kernel

namespace OpenEra::Graphics
{
// Registers desktop graphics shims that put the preferred GPU (SelectAdapter
// with CurrentGpuPreference) in front of titles, on NVIDIA and AMD alike:
// - dxgi.dll: CreateDXGIFactory, CreateDXGIFactory1 and CreateDXGIFactory2
//   return factories that enumerate the preferred adapter first.
// - d3d12.dll: D3D12CreateDevice with no adapter uses the preferred one.
// Other exports of these modules pass through to the host's own DLLs.
bool RegisterGraphicsShims(Kernel::ShimRegistry& shims);
} // namespace OpenEra::Graphics
