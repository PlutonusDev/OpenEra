// Runs against the machine's real DXGI adapters, whatever they are (on CI only
// the software adapter), comparing the shims with SelectAdapter's choice.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "Core/Logger.h"
#include "Graphics/AdapterSelection.h"
#include "Graphics/DxgiFactory.h"
#include "Graphics/GraphicsShims.h"
#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"

using namespace OpenEra::Graphics;
using Microsoft::WRL::ComPtr;
using OpenEra::Kernel::FromShimProc;
using OpenEra::Kernel::ShimRegistry;

namespace
{
using CreateFactory2Function = HRESULT WINAPI(UINT, REFIID, void**);
using CreateDeviceFunction = HRESULT WINAPI(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

template <typename Function>
Function* Shim(const ShimRegistry& shims, std::string_view module, std::string_view symbol)
{
    const auto resolved = shims.Resolve(module, symbol);
    REQUIRE(resolved.has_value());
    return FromShimProc<Function>(resolved->Export->Proc);
}

uint64_t LuidOf(IDXGIAdapter1& adapter)
{
    DXGI_ADAPTER_DESC1 desc{};
    REQUIRE(SUCCEEDED(adapter.GetDesc1(&desc)));
    return std::bit_cast<uint64_t>(desc.AdapterLuid);
}

// What SelectAdapter picks from the real adapters, with the default preference.
std::optional<uint64_t> ExpectedLuid()
{
    ComPtr<IDXGIFactory1> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    const auto adapters = DescribeAdapters(*factory.Get());
    const auto chosen = SelectAdapter(adapters, {});
    return chosen ? std::optional(adapters.at(*chosen).Luid) : std::nullopt;
}

class GraphicsFixture
{
public:
    GraphicsFixture()
    {
        SetGpuPreference({});
        REQUIRE(RegisterGraphicsShims(m_shims));
    }

    [[nodiscard]] const ShimRegistry& Shims() const noexcept { return m_shims; }

private:
    ShimRegistry m_shims;
};
} // namespace

TEST_CASE("Shimmed DXGI factories list the preferred GPU first", "[graphics][dxgi]")
{
    const GraphicsFixture fixture;
    auto* const create = Shim<CreateFactory2Function>(fixture.Shims(), "dxgi.dll", "CreateDXGIFactory2");

    ComPtr<IDXGIFactory1> factory;
    REQUIRE(SUCCEEDED(create(0, IID_PPV_ARGS(&factory))));
    const auto expected = ExpectedLuid();
    ComPtr<IDXGIAdapter1> first;
    if (!expected)
    {
        CHECK(factory->EnumAdapters1(0, &first) == DXGI_ERROR_NOT_FOUND);
        return;
    }
    REQUIRE(SUCCEEDED(factory->EnumAdapters1(0, &first)));
    CHECK(LuidOf(*first.Get()) == *expected);

    // The same adapters, each exactly once.
    ComPtr<IDXGIFactory1> real;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&real))));
    const auto realAdapters = DescribeAdapters(*real.Get());
    const auto wrappedAdapters = DescribeAdapters(*factory.Get());
    REQUIRE(wrappedAdapters.size() == realAdapters.size());
    for (const auto& adapter : realAdapters)
    {
        CHECK(std::ranges::count_if(wrappedAdapters, [&adapter](const AdapterInfo& wrapped)
                                    { return wrapped.Luid == adapter.Luid; }) == 1);
    }

    // DXGI_GPU_PREFERENCE_UNSPECIFIED follows the same order.
    ComPtr<IDXGIFactory6> factory6;
    REQUIRE(SUCCEEDED(factory.As(&factory6)));
    ComPtr<IDXGIAdapter1> unspecified;
    REQUIRE(SUCCEEDED(
        factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_UNSPECIFIED, IID_PPV_ARGS(&unspecified))));
    CHECK(LuidOf(*unspecified.Get()) == *expected);
}

TEST_CASE("Shimmed DXGI factories keep COM identity and reference counts", "[graphics][dxgi]")
{
    const GraphicsFixture fixture;
    auto* const create = Shim<CreateFactory2Function>(fixture.Shims(), "dxgi.dll", "CreateDXGIFactory2");

    ComPtr<IDXGIFactory2> factory;
    REQUIRE(SUCCEEDED(create(0, IID_PPV_ARGS(&factory))));
    ComPtr<IUnknown> unknownA;
    ComPtr<IUnknown> unknownB;
    ComPtr<IDXGIFactory4> factory4;
    REQUIRE(SUCCEEDED(factory.As(&unknownA)));
    REQUIRE(SUCCEEDED(factory.As(&factory4)));
    REQUIRE(SUCCEEDED(factory4.As(&unknownB)));
    CHECK(unknownA.Get() == unknownB.Get());

    factory->AddRef();
    CHECK(factory->Release() >= 1);
    void* nothing = nullptr;
    CHECK(factory->QueryInterface(__uuidof(ID3D12Device), &nothing) == E_NOINTERFACE);
    CHECK(nothing == nullptr);
}

TEST_CASE("D3D12CreateDevice without an adapter uses the preferred GPU", "[graphics][d3d12]")
{
    const GraphicsFixture fixture;
    auto* const create = Shim<CreateDeviceFunction>(fixture.Shims(), "d3d12.dll", "D3D12CreateDevice");
    const auto expected = ExpectedLuid();
    if (!expected)
    {
        SKIP("no DXGI adapters");
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    {
        SKIP("the preferred adapter has no Direct3D 12 driver");
    }
    CHECK(std::bit_cast<uint64_t>(device->GetAdapterLuid()) == *expected);
}

TEST_CASE("The console device is a recording stand-in", "[graphics][console]")
{
    OpenEra::Core::Logger log;
    OpenEra::Kernel::StubRegistry stubs(log);
    ShimRegistry shims;
    REQUIRE(RegisterGraphicsShims(shims, &stubs));
    using CreateConsoleDevice = HRESULT WINAPI(IUnknown*, const void*, REFIID, void**);
    auto* const create = Shim<CreateConsoleDevice>(shims, "d3d12_xs.dll", "D3D12XboxCreateDevice");
    CHECK(Shim<CreateConsoleDevice>(shims, "d3d12_x.dll", "D3D12XboxCreateDevice") == create);

    void* raw = nullptr;
    REQUIRE(create(nullptr, nullptr, __uuidof(ID3D12Device), &raw) == S_OK);
    REQUIRE(raw != nullptr);
    auto* const device = static_cast<IUnknown*>(raw);

    // Any other slot records itself and fails cleanly.
    using Slot = HRESULT(STDMETHODCALLTYPE*)(void*, uint64_t, uint64_t, uint64_t);
    const auto* const vtable = *static_cast<Slot* const*>(raw);
    CHECK(vtable[8](raw, 1, 2, 3) == E_NOTIMPL);   // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    CHECK(vtable[8](raw, 0, 0, 0) == E_NOTIMPL);   // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    CHECK(vtable[255](raw, 0, 0, 0) == E_NOTIMPL); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    using OpenEra::Kernel::StubKind;
    CHECK(stubs.GetHitCount(StubKind::Win32Import, "d3d12_xs.dll", "device::slot008") == 2);
    CHECK(stubs.GetHitCount(StubKind::Win32Import, "d3d12_xs.dll", "device::slot255") == 1);
    CHECK(stubs.GetHitCount(StubKind::Win32Import, "d3d12_xs.dll",
                            "D3D12XboxCreateDevice {189819F1-1DB6-4B57-BE54-1821339B85F7}") == 1);

    // COM identity and reference counting.
    void* again = nullptr;
    REQUIRE(device->QueryInterface(__uuidof(ID3D12Device), &again) == S_OK);
    CHECK(again == raw);
    CHECK(stubs.GetHitCount(StubKind::Win32Import, "d3d12_xs.dll",
                            "device::QueryInterface {189819F1-1DB6-4B57-BE54-1821339B85F7}") == 1);
    CHECK(device->QueryInterface(__uuidof(ID3D12Device), nullptr) == E_POINTER);
    CHECK(device->AddRef() == 3);
    CHECK(device->Release() == 2);
    CHECK(device->Release() == 1);
    CHECK(device->Release() == 0); // Freed.
}
