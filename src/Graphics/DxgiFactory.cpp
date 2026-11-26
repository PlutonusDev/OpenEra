#include "DxgiFactory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <exception>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>

#include <wrl/implements.h>

#include "Core/Logger.h"

namespace OpenEra::Graphics
{
namespace
{
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

constexpr uint64_t kMiB = 1024ULL * 1024;

UINT LowDword(size_t value) noexcept
{
    return std::bit_cast<std::array<UINT, 2>>(value).front();
}

std::string Narrow(const std::wstring& text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
    {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), length, nullptr, nullptr);
    result.pop_back(); // Terminator.
    return result;
}

AdapterInfo Describe(IDXGIAdapter1& adapter)
{
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter.GetDesc1(&desc)))
    {
        return {};
    }
    const auto& name = desc.Description;
    const std::wstring description(std::begin(name), std::ranges::find(name, L'\0'));
    constexpr UINT kSoftwareFlag = DXGI_ADAPTER_FLAG_SOFTWARE;
    return {.Description = Narrow(description),
            .VendorId = desc.VendorId,
            .DeviceId = desc.DeviceId,
            .Luid = std::bit_cast<uint64_t>(desc.AdapterLuid),
            .DedicatedVideoMemory = desc.DedicatedVideoMemory,
            .Software = (desc.Flags & kSoftwareFlag) != 0};
}

// Adapter indices with the preferred one first, the rest in DXGI's order.
std::vector<UINT> PreferredOrder(const std::vector<AdapterInfo>& adapters, bool logChoice)
{
    std::vector<UINT> order;
    order.reserve(adapters.size());
    const auto preferred = SelectAdapter(adapters, CurrentGpuPreference());
    if (preferred)
    {
        order.push_back(LowDword(*preferred));
        if (logChoice)
        {
            const auto& adapter = adapters.at(*preferred);
            Core::LogInfo("Graphics: using {} ({}, {} MiB dedicated)", adapter.Description,
                          VendorName(adapter.VendorId), adapter.DedicatedVideoMemory / kMiB);
        }
    }
    for (size_t index = 0; index < adapters.size(); ++index)
    {
        if (!preferred || index != *preferred)
        {
            order.push_back(LowDword(index));
        }
    }
    return order;
}

bool IsFactoryInterface(REFIID riid) noexcept
{
    static const std::array<IID, 9> kInterfaces = {
        __uuidof(IUnknown),      __uuidof(IDXGIObject),   __uuidof(IDXGIFactory),
        __uuidof(IDXGIFactory1), __uuidof(IDXGIFactory2), __uuidof(IDXGIFactory3),
        __uuidof(IDXGIFactory4), __uuidof(IDXGIFactory5), __uuidof(IDXGIFactory6)};
    return std::ranges::find(kInterfaces, riid) != kInterfaces.end() || riid == __uuidof(IDXGIFactory7);
}

// Forwards to a real factory, reordering adapter enumeration.
class PreferredGpuFactory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IDXGIFactory7>
{
public:
    PreferredGpuFactory(ComPtr<IDXGIFactory6> inner, ComPtr<IDXGIFactory7> inner7, std::vector<UINT> order) noexcept
        : m_inner(std::move(inner))
        , m_inner7(std::move(inner7))
        , m_order(std::move(order))
    {
    }

    // noexcept like the WRL base; the inner QueryInterface it calls doesn't throw.
    [[gsl::suppress("f.6")]] HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }
        *object = nullptr;
        if (IsFactoryInterface(riid))
        {
            if (riid == __uuidof(IDXGIFactory7) && !m_inner7)
            {
                return E_NOINTERFACE;
            }
            *object = static_cast<IDXGIFactory7*>(this);
            AddRef();
            return S_OK;
        }
        return m_inner->QueryInterface(riid, object);
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void* data) override
    {
        return m_inner->SetPrivateData(name, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown* unknown) override
    {
        return m_inner->SetPrivateDataInterface(name, unknown);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT* size, void* data) override
    {
        return m_inner->GetPrivateData(name, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override
    {
        return m_inner->GetParent(riid, parent);
    }

    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT index, IDXGIAdapter** adapter) override
    {
        return m_inner->EnumAdapters(Map(index), adapter);
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND window, UINT flags) override
    {
        return m_inner->MakeWindowAssociation(window, flags);
    }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* window) override
    {
        return m_inner->GetWindowAssociation(window);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** swapChain) override
    {
        return m_inner->CreateSwapChain(device, desc, swapChain);
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE module, IDXGIAdapter** adapter) override
    {
        return m_inner->CreateSoftwareAdapter(module, adapter);
    }

    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1** adapter) override
    {
        return m_inner->EnumAdapters1(Map(index), adapter);
    }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return m_inner->IsCurrent(); }

    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return m_inner->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                     IDXGIOutput* restrictToOutput,
                                                     IDXGISwapChain1** swapChain) override
    {
        return m_inner->CreateSwapChainForHwnd(device, window, desc, fullscreenDesc, restrictToOutput, swapChain);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* device, IUnknown* window,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc,
                                                           IDXGIOutput* restrictToOutput,
                                                           IDXGISwapChain1** swapChain) override
    {
        return m_inner->CreateSwapChainForCoreWindow(device, window, desc, restrictToOutput, swapChain);
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE resource, LUID* luid) override
    {
        return m_inner->GetSharedResourceAdapterLuid(resource, luid);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND window, UINT message, DWORD* cookie) override
    {
        return m_inner->RegisterStereoStatusWindow(window, message, cookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE event, DWORD* cookie) override
    {
        return m_inner->RegisterStereoStatusEvent(event, cookie);
    }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD cookie) override { m_inner->UnregisterStereoStatus(cookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND window, UINT message, DWORD* cookie) override
    {
        return m_inner->RegisterOcclusionStatusWindow(window, message, cookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE event, DWORD* cookie) override
    {
        return m_inner->RegisterOcclusionStatusEvent(event, cookie);
    }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD cookie) override
    {
        m_inner->UnregisterOcclusionStatus(cookie);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                            IDXGIOutput* restrictToOutput,
                                                            IDXGISwapChain1** swapChain) override
    {
        return m_inner->CreateSwapChainForComposition(device, desc, restrictToOutput, swapChain);
    }

    // IDXGIFactory3
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return m_inner->GetCreationFlags(); }

    // IDXGIFactory4
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID riid, void** adapter) override
    {
        return m_inner->EnumAdapterByLuid(luid, riid, adapter);
    }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** adapter) override
    {
        return m_inner->EnumWarpAdapter(riid, adapter);
    }

    // IDXGIFactory5
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void* data, UINT size) override
    {
        return m_inner->CheckFeatureSupport(feature, data, size);
    }

    // IDXGIFactory6: an unspecified preference follows OpenEra's order.
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT index, DXGI_GPU_PREFERENCE preference, REFIID riid,
                                                         void** adapter) override
    {
        if (preference != DXGI_GPU_PREFERENCE_UNSPECIFIED || adapter == nullptr)
        {
            return m_inner->EnumAdapterByGpuPreference(index, preference, riid, adapter);
        }
        *adapter = nullptr;
        ComPtr<IDXGIAdapter1> found;
        const HRESULT result = m_inner->EnumAdapters1(Map(index), &found);
        return FAILED(result) ? result : found->QueryInterface(riid, adapter);
    }

    // IDXGIFactory7
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE event, DWORD* cookie) override
    {
        return m_inner7 ? m_inner7->RegisterAdaptersChangedEvent(event, cookie) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD cookie) override
    {
        return m_inner7 ? m_inner7->UnregisterAdaptersChangedEvent(cookie) : E_NOTIMPL;
    }

private:
    [[nodiscard]] UINT Map(UINT index) const { return index < m_order.size() ? m_order.at(index) : index; }

    ComPtr<IDXGIFactory6> m_inner;
    ComPtr<IDXGIFactory7> m_inner7;
    std::vector<UINT> m_order;
};
} // namespace

std::vector<AdapterInfo> DescribeAdapters(IDXGIFactory1& factory)
{
    std::vector<AdapterInfo> adapters;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory.EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
    {
        adapters.push_back(adapter ? Describe(*adapter.Get()) : AdapterInfo{});
    }
    return adapters;
}

ComPtr<IDXGIAdapter1> PreferredAdapter(IDXGIFactory1& factory)
{
    const auto preferred = SelectAdapter(DescribeAdapters(factory), CurrentGpuPreference());
    ComPtr<IDXGIAdapter1> adapter;
    if (preferred)
    {
        std::ignore = factory.EnumAdapters1(LowDword(*preferred), &adapter);
    }
    return adapter;
}

HRESULT WrapFactory(IDXGIFactory1& inner, REFIID riid, void** factory)
{
    if (factory == nullptr)
    {
        return E_POINTER;
    }
    *factory = nullptr;
    const ComPtr<IDXGIFactory1> holder(&inner);
    try
    {
        ComPtr<IDXGIFactory6> inner6;
        if (FAILED(holder.As(&inner6)))
        {
            return inner.QueryInterface(riid, factory); // Too old to wrap: no reordering.
        }
        ComPtr<IDXGIFactory7> inner7;
        std::ignore = holder.As(&inner7);
        static std::atomic<bool> logged{false};
        auto order = PreferredOrder(DescribeAdapters(inner), !logged.exchange(true));
        const auto wrapper = Make<PreferredGpuFactory>(std::move(inner6), std::move(inner7), std::move(order));
        if (!wrapper)
        {
            return E_OUTOFMEMORY;
        }
        return wrapper->QueryInterface(riid, factory);
    }
    catch (const std::exception&)
    {
        return inner.QueryInterface(riid, factory);
    }
}
} // namespace OpenEra::Graphics
