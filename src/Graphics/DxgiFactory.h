#pragma once

#include <vector>

#include <dxgi1_6.h>
#include <wrl/client.h>

#include "Graphics/AdapterSelection.h"

namespace OpenEra::Graphics
{
// The factory's adapters in DXGI's own order (EnumAdapters1).
[[nodiscard]] std::vector<AdapterInfo> DescribeAdapters(IDXGIFactory1& factory);

// The adapter SelectAdapter picks with the current preference, or null if there's none.
[[nodiscard]] Microsoft::WRL::ComPtr<IDXGIAdapter1> PreferredAdapter(IDXGIFactory1& factory);

// Wraps a DXGI factory so EnumAdapters / EnumAdapters1 (and EnumAdapterByGpuPreference
// with DXGI_GPU_PREFERENCE_UNSPECIFIED) list the preferred GPU first. Everything
// else forwards to `inner`. Returns `inner` itself if it can't be wrapped.
[[nodiscard]] HRESULT WrapFactory(IDXGIFactory1& inner, REFIID riid, void** factory);
} // namespace OpenEra::Graphics
