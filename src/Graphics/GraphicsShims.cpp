#include "GraphicsShims.h"

#include <bit>
#include <string_view>
#include <utility>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "Graphics/DxgiFactory.h"
#include "Kernel/ShimGuard.h"
#include "Kernel/ShimModule.h"

namespace OpenEra::Graphics
{
namespace
{
using Kernel::HResult;
using Microsoft::WRL::ComPtr;

// HRESULT is long and Kernel::HResult is int32_t: the same 32 bits.
HRESULT ToHresult(HResult value) noexcept
{
    return std::bit_cast<HRESULT>(value);
}

HResult FromHresult(HRESULT value) noexcept
{
    return std::bit_cast<HResult>(value);
}

// Creates the real factory with `create`, then wraps it.
template <typename Create>
HRESULT CreateWrappedFactory(std::string_view name, REFIID riid, void** factory, Create create) noexcept
{
    return ToHresult(Kernel::GuardHResult(name,
                                          [&]() -> HResult
                                          {
                                              ComPtr<IDXGIFactory1> inner;
                                              const HRESULT result = create(inner);
                                              if (FAILED(result) || !inner)
                                              {
                                                  if (factory != nullptr)
                                                  {
                                                      *factory = nullptr;
                                                  }
                                                  return FromHresult(FAILED(result) ? result : E_FAIL);
                                              }
                                              return FromHresult(WrapFactory(*inner.Get(), riid, factory));
                                          }));
}

[[gsl::suppress("type.1")]] HRESULT WINAPI CreateDXGIFactoryShim(REFIID riid, void** factory) noexcept
{
    return CreateWrappedFactory("CreateDXGIFactory", riid, factory, [](ComPtr<IDXGIFactory1>& inner) noexcept
                                { return CreateDXGIFactory(IID_PPV_ARGS(&inner)); });
}

[[gsl::suppress("type.1")]] HRESULT WINAPI CreateDXGIFactory1Shim(REFIID riid, void** factory) noexcept
{
    return CreateWrappedFactory("CreateDXGIFactory1", riid, factory, [](ComPtr<IDXGIFactory1>& inner) noexcept
                                { return CreateDXGIFactory1(IID_PPV_ARGS(&inner)); });
}

[[gsl::suppress("type.1")]] HRESULT WINAPI CreateDXGIFactory2Shim(UINT flags, REFIID riid, void** factory) noexcept
{
    return CreateWrappedFactory("CreateDXGIFactory2", riid, factory, [flags](ComPtr<IDXGIFactory1>& inner) noexcept
                                { return CreateDXGIFactory2(flags, IID_PPV_ARGS(&inner)); });
}

[[gsl::suppress("type.1")]] HRESULT WINAPI D3D12CreateDeviceShim(IUnknown* adapter, D3D_FEATURE_LEVEL minimumLevel,
                                                                 REFIID riid, void** device) noexcept
{
    return ToHresult(Kernel::GuardHResult("D3D12CreateDevice",
                                          [&]() -> HResult
                                          {
                                              ComPtr<IDXGIAdapter1> preferred;
                                              if (adapter == nullptr)
                                              {
                                                  ComPtr<IDXGIFactory1> factory;
                                                  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                                                  {
                                                      preferred = PreferredAdapter(*factory.Get());
                                                  }
                                              }
                                              IUnknown* const chosen = adapter != nullptr ? adapter : preferred.Get();
                                              return FromHresult(D3D12CreateDevice(chosen, minimumLevel, riid, device));
                                          }));
}
} // namespace

bool RegisterGraphicsShims(Kernel::ShimRegistry& shims)
{
    Kernel::ShimModule dxgi("dxgi.dll", Kernel::StubKind::Win32Import);
    const bool dxgiExported = dxgi.Export("CreateDXGIFactory", Kernel::ToShimProc(&CreateDXGIFactoryShim)) &&
                              dxgi.Export("CreateDXGIFactory1", Kernel::ToShimProc(&CreateDXGIFactory1Shim)) &&
                              dxgi.Export("CreateDXGIFactory2", Kernel::ToShimProc(&CreateDXGIFactory2Shim));

    Kernel::ShimModule d3d12("d3d12.dll", Kernel::StubKind::Win32Import);
    const bool d3d12Exported = d3d12.Export("D3D12CreateDevice", Kernel::ToShimProc(&D3D12CreateDeviceShim));

    return dxgiExported && d3d12Exported && shims.Register(std::move(dxgi)) && shims.Register(std::move(d3d12));
}
} // namespace OpenEra::Graphics
