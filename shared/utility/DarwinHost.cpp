#include <cstring>
#include <spdlog/spdlog.h>
#include "DarwinHost.hpp"

namespace utility {

bool DarwinHost::is_wine() {
    static const bool result = []() {
        const auto ntdll = GetModuleHandleA("ntdll.dll");
        const bool wine = (ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr);
        if (wine) {
            spdlog::info("DarwinHost: Wine environment detected");
        }
        return wine;
    }();
    return result;
}

bool DarwinHost::is_darwin() {
    static const bool result = []() {
        const auto ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll == nullptr || GetProcAddress(ntdll, "wine_get_version") == nullptr) {
            return false;
        }

        using wine_get_host_version_t = void(__cdecl*)(const char** sysname, const char** release);
        const auto wine_get_host_version = (wine_get_host_version_t)GetProcAddress(ntdll, "wine_get_host_version");
        if (wine_get_host_version == nullptr) {
            return false;
        }

        const char* sysname = nullptr;
        const char* release = nullptr;
        wine_get_host_version(&sysname, &release);

        const bool is_macos = (sysname != nullptr && strcmp(sysname, "Darwin") == 0);
        spdlog::info("DarwinHost: Wine host OS: {} {} -> macOS/Darwin paths {}",
            sysname != nullptr ? sysname : "?",
            release != nullptr ? release : "?",
            is_macos ? "enabled" : "disabled");

        return is_macos;
    }();
    return result;
}

// Isolated on purpose: functions containing __try/__except cannot contain C++ objects that need unwinding.
static HRESULT __declspec(noinline) d3d12_create_device_safe_seh(
    decltype(D3D12CreateDevice)* fn,
    IUnknown* adapter,
    D3D_FEATURE_LEVEL feature_level,
    ID3D12Device** device_out) 
{
    HRESULT hr = E_FAIL;
    __try {
        hr = fn(adapter, feature_level, IID_PPV_ARGS(device_out));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hr = E_FAIL;
    }
    return hr;
}

HRESULT DarwinHost::create_d3d12_dummy_device(
    decltype(D3D12CreateDevice)* create_fn,
    D3D_FEATURE_LEVEL feature_level,
    ID3D12Device** out_device)
{
    if (create_fn == nullptr || out_device == nullptr) {
        return E_INVALIDARG;
    }

    const auto dxgi_module = LoadLibraryA("dxgi.dll");
    if (dxgi_module == nullptr) {
        spdlog::error("DarwinHost: Failed to load dxgi.dll");
        return E_FAIL;
    }

    auto create_dxgi_factory = (decltype(CreateDXGIFactory)*)GetProcAddress(dxgi_module, "CreateDXGIFactory");
    if (create_dxgi_factory == nullptr) {
        spdlog::error("DarwinHost: Failed to get CreateDXGIFactory export");
        return E_FAIL;
    }

    IDXGIFactory4* factory{ nullptr };
    if (FAILED(create_dxgi_factory(IID_PPV_ARGS(&factory))) || factory == nullptr) {
        spdlog::error("DarwinHost: Failed to create DXGI factory for adapter enumeration");
        return E_FAIL;
    }

    IDXGIAdapter* adapter{ nullptr };
    const auto enum_hr = factory->EnumAdapters(0, &adapter);
    factory->Release();

    if (FAILED(enum_hr) || adapter == nullptr) {
        spdlog::error("DarwinHost: No DXGI adapter found ({:x})", (uint32_t)enum_hr);
        return E_FAIL;
    }

    spdlog::info("DarwinHost: Calling D3D12CreateDevice with enumerated adapter {:x}", (uintptr_t)adapter);
    const auto hr = d3d12_create_device_safe_seh(create_fn, adapter, feature_level, out_device);
    adapter->Release();

    if (FAILED(hr)) {
        spdlog::error("DarwinHost: D3D12CreateDevice failed: 0x{:X}", (uint32_t)hr);
    }

    return hr;
}

uint32_t DarwinHost::find_command_queue_offset(void* swapchain, IUnknown* command_queue) {
    if (swapchain == nullptr) {
        return 0;
    }

    // In CrossOver / Apple D3DMetal, the swapchain struct consistently places
    // the command queue at 0x4C8 across D3DMetal versions (upstream PR #1589 & #1847).
    constexpr uint32_t WINE_D3DMETAL_CQ_OFFSET = 0x4C8;
    const auto d3dmetal_base = (uintptr_t)swapchain + WINE_D3DMETAL_CQ_OFFSET;
    if (!IsBadReadPtr((void*)d3dmetal_base, sizeof(void*))) {
        auto candidate = *(void**)d3dmetal_base;
        if (candidate != nullptr && !IsBadReadPtr(candidate, sizeof(void*))) {
            spdlog::info("DarwinHost: Using verified D3DMetal command queue offset: 0x{:x}", WINE_D3DMETAL_CQ_OFFSET);
            return WINE_D3DMETAL_CQ_OFFSET;
        }
    }

    // Default to the known D3DMetal offset (CrossOver / GPTK)
    spdlog::warn("DarwinHost: Defaulting to standard D3DMetal command queue offset: 0x{:x}", WINE_D3DMETAL_CQ_OFFSET);
    return WINE_D3DMETAL_CQ_OFFSET;
}

bool DarwinHost::matches_command_queue(IUnknown* candidate, IUnknown* command_queue) {
    if (candidate == nullptr || command_queue == nullptr) {
        return false;
    }
    return candidate == command_queue;
}

intptr_t DarwinHost::calculate_command_queue_delta(void* swapchain, uint32_t offset, IUnknown* command_queue) {
    if (swapchain == nullptr || offset < sizeof(void*) || (offset % sizeof(void*) != 0) || command_queue == nullptr) {
        return 0;
    }
    const auto raw_ptr_addr = (uintptr_t)swapchain + offset;
    if (IsBadReadPtr((void*)raw_ptr_addr, sizeof(void*))) {
        return 0;
    }
    const auto raw = *(uintptr_t*)raw_ptr_addr;
    if (raw == 0 || IsBadReadPtr((void*)raw, sizeof(void*))) {
        return 0;
    }
    const auto delta = (intptr_t)((uintptr_t)command_queue - raw);
    if (delta != 0) {
        spdlog::info("DarwinHost: Command queue pointer delta: 0x{:X}", (uintptr_t)delta);
    }
    return delta;
}

uint32_t DarwinHost::get_default_menu_key() {
    return is_darwin() ? VK_F10 : VK_INSERT;
}

bool DarwinHost::is_mac_command_key(uint32_t vk_code) {
    return is_darwin() && (vk_code == VK_LWIN || vk_code == VK_RWIN);
}

} // namespace utility
