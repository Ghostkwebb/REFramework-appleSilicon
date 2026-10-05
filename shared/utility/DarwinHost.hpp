#pragma once

#include <cstdint>
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>
#include <dxgi1_4.h>

namespace utility {
class DarwinHost {
public:
    // Wine & macOS (Darwin) host environment detection
    static bool is_wine();
    static bool is_darwin();

    // D3D12 / D3DMetal helpers for macOS CrossOver / GPTK
    static HRESULT create_d3d12_dummy_device(
        decltype(D3D12CreateDevice)* create_fn,
        D3D_FEATURE_LEVEL feature_level,
        ID3D12Device** out_device
    );

    static uint32_t find_command_queue_offset(void* swapchain, IUnknown* command_queue);
    static bool matches_command_queue(IUnknown* candidate, IUnknown* command_queue);
    static intptr_t calculate_command_queue_delta(void* swapchain, uint32_t offset, IUnknown* command_queue);

    // Keyboard & input helpers
    static uint32_t get_default_menu_key();
    static bool is_mac_command_key(uint32_t vk_code);
};
}
