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

        if (is_macos) {
            SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
            spdlog::info("DarwinHost: Configured SetThreadExecutionState (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)");
        }

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

    // 1. Dynamic scan: If command_queue is known, scan swapchain memory for the exact pointer match
    if (command_queue != nullptr) {
        for (uint32_t off = 0; off <= 0x800 - sizeof(void*); off += sizeof(void*)) {
            const auto addr = (uintptr_t)swapchain + off;
            if (IsBadReadPtr((void*)addr, sizeof(void*))) {
                break;
            }
            auto cand = *(void**)addr;
            if (cand == (void*)command_queue) {
                spdlog::info("DarwinHost: Dynamically matched exact command queue at offset 0x{:X}", off);
                return off;
            }
        }
    }

    // 2. Standard D3DMetal fallback (0x4C8 across GPTK 3/4)
    constexpr uint32_t WINE_D3DMETAL_CQ_OFFSET = 0x4C8;
    const auto d3dmetal_base = (uintptr_t)swapchain + WINE_D3DMETAL_CQ_OFFSET;
    if (!IsBadReadPtr((void*)d3dmetal_base, sizeof(void*))) {
        auto candidate = *(void**)d3dmetal_base;
        if (candidate != nullptr && !IsBadReadPtr(candidate, sizeof(void*))) {
            spdlog::info("DarwinHost: Using verified D3DMetal command queue offset: 0x{:X}", WINE_D3DMETAL_CQ_OFFSET);
            return WINE_D3DMETAL_CQ_OFFSET;
        }
    }

    spdlog::warn("DarwinHost: Defaulting to standard D3DMetal command queue offset: 0x{:X}", WINE_D3DMETAL_CQ_OFFSET);
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

bool DarwinHost::is_host_darwin_pointer(uintptr_t addr) {
    if (addr < 0x10000ULL) {
        return true;
    }
    // Native Objective-C / Metal heap objects: 0x600000000000+
    if ((addr & 0xFFFFFF0000000000ULL) == 0x600000000000ULL) {
        return true;
    }
    // macOS thread stacks: 0x700000000000 - 0x7000ffffffff
    if ((addr & 0xFFFFFF0000000000ULL) == 0x700000000000ULL) {
        return true;
    }
    // macOS dyld shared cache / system dylibs: 0x7ff800000000+
    if (addr >= 0x7ff800000000ULL) {
        return true;
    }
    return false;
}

bool DarwinHost::is_valid_command_queue(ID3D12CommandQueue* queue) {
    if (queue == nullptr || IsBadReadPtr(queue, sizeof(void*))) {
        return false;
    }
    const auto ptr_val = (uintptr_t)queue;
    if (is_darwin() && is_host_darwin_pointer(ptr_val)) {
        return false;
    }
    void** vtable = *(void***)queue;
    if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*) * 10)) {
        return false;
    }
    const auto vtable_val = (uintptr_t)vtable;
    if (is_darwin() && is_host_darwin_pointer(vtable_val)) {
        return false;
    }
    const auto fn = (uintptr_t)vtable[0];
    if (is_darwin() && is_host_darwin_pointer(fn)) {
        return false;
    }
    bool ok = false;
    __try {
        IUnknown* test = nullptr;
        if (SUCCEEDED(queue->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&test)) && test != nullptr) {
            test->Release();
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

uint32_t DarwinHost::get_default_menu_key() {
    return is_darwin() ? VK_OEM_3 : VK_INSERT;
}

bool DarwinHost::is_mac_command_key(uint32_t vk_code) {
    return is_darwin() && (vk_code == VK_LWIN || vk_code == VK_RWIN);
}

// SysV AMD64 calling convention thunks for macOS native functions
static int call_sysv_setenv(void* fn, const char* name, const char* value, int overwrite) {
    if (fn == nullptr) {
        return -1;
    }

    // Windows x64 ABI (caller): rcx = fn, rdx = name, r8 = value, r9 = overwrite
    // SysV AMD64 ABI (callee):  rdi = name, rsi = value, rdx = overwrite, rax = fn
    // Stack must be 16-byte aligned before call
    static const uint8_t thunk_code[] = {
        0x55,                                     // push rbp
        0x48, 0x89, 0xe5,                         // mov rbp, rsp
        0x53,                                     // push rbx
        0x57,                                     // push rdi
        0x56,                                     // push rsi
        0x48, 0x89, 0xc8,                         // mov rax, rcx  (fn)
        0x48, 0x89, 0xd7,                         // mov rdi, rdx  (name)
        0x48, 0x89, 0xc6,                         // mov rsi, r8   (value)
        0x48, 0x89, 0xca,                         // mov rdx, r9   (overwrite)
        0x48, 0x83, 0xe4, 0xf0,                   // and rsp, -16  (16-byte stack align)
        0xff, 0xd0,                               // call rax
        0x48, 0x8d, 0x65, 0xe8,                   // lea rsp, [rbp - 24]
        0x5e,                                     // pop rsi
        0x5f,                                     // pop rdi
        0x5b,                                     // pop rbx
        0x5d,                                     // pop rbp
        0xc3                                      // ret
    };

    static void* executable_thunk = nullptr;
    if (executable_thunk == nullptr) {
        executable_thunk = VirtualAlloc(nullptr, sizeof(thunk_code), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (executable_thunk != nullptr) {
            memcpy(executable_thunk, thunk_code, sizeof(thunk_code));
        }
    }

    if (executable_thunk == nullptr) {
        return -1;
    }

    using thunk_fn_t = int (*)(void*, const char*, const char*, int);
    return ((thunk_fn_t)executable_thunk)(fn, name, value, overwrite);
}

static void* call_sysv_dlsym(void* fn, void* handle, const char* name) {
    if (fn == nullptr) {
        return nullptr;
    }

    // Windows x64 ABI (caller): rcx = fn, rdx = handle, r8 = name
    // SysV AMD64 ABI (callee):  rdi = handle, rsi = name, rax = fn
    static const uint8_t thunk_code[] = {
        0x55,                                     // push rbp
        0x48, 0x89, 0xe5,                         // mov rbp, rsp
        0x53,                                     // push rbx
        0x57,                                     // push rdi
        0x56,                                     // push rsi
        0x48, 0x89, 0xc8,                         // mov rax, rcx  (fn)
        0x48, 0x89, 0xd7,                         // mov rdi, rdx  (handle)
        0x48, 0x89, 0xc6,                         // mov rsi, r8   (name)
        0x48, 0x83, 0xe4, 0xf0,                   // and rsp, -16  (16-byte stack align)
        0xff, 0xd0,                               // call rax
        0x48, 0x8d, 0x65, 0xe8,                   // lea rsp, [rbp - 24]
        0x5e,                                     // pop rsi
        0x5f,                                     // pop rdi
        0x5b,                                     // pop rbx
        0x5d,                                     // pop rbp
        0xc3                                      // ret
    };

    static void* executable_thunk = nullptr;
    if (executable_thunk == nullptr) {
        executable_thunk = VirtualAlloc(nullptr, sizeof(thunk_code), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (executable_thunk != nullptr) {
            memcpy(executable_thunk, thunk_code, sizeof(thunk_code));
        }
    }

    if (executable_thunk == nullptr) {
        return nullptr;
    }

    using thunk_fn_t = void* (*)(void*, void*, const char*);
    return ((thunk_fn_t)executable_thunk)(fn, handle, name);
}

// Memory scanner & enforcer wrapped in SEH (__try/__except cannot contain C++ objects requiring destruction)
static bool __declspec(noinline) enforce_metal3_mode_seh(
    uintptr_t& out_d3dmetal_base,
    uintptr_t& out_setenv_fn,
    int& out_setenv_res,
    uintptr_t& out_mtl4_addr,
    uint8_t& out_prior_val)
{
    __try {
        static const char target_str[] = "D3DM_MTL4";
        const size_t target_len = sizeof(target_str); // 10 bytes including null terminator

        uintptr_t str_addr = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t scan_ptr = 0x200000000ULL;
        const uintptr_t scan_end = 0x400000000ULL;

        // 1. Locate string "D3DM_MTL4" in committed readable memory
        while (scan_ptr < scan_end && VirtualQuery((LPCVOID)scan_ptr, &mbi, sizeof(mbi)) != 0) {
            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                !(mbi.Protect & PAGE_GUARD))
            {
                const uint8_t* start = (const uint8_t*)mbi.BaseAddress;
                const size_t size = mbi.RegionSize;
                if (size >= target_len) {
                    const uint8_t* p_end = start + size - target_len;
                    for (const uint8_t* p = start; p <= p_end; ++p) {
                        if (*p == 'D' && memcmp(p, target_str, target_len) == 0) {
                            str_addr = (uintptr_t)p;
                            break;
                        }
                    }
                }
            }
            if (str_addr != 0) {
                break;
            }
            scan_ptr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        }

        if (str_addr == 0) {
            return false;
        }

        // 2. Find Mach-O header of D3DMetal (0xFEEDFACF) by walking backward page by page
        uintptr_t d3dmetal_base = 0;
        uintptr_t page_candidate = str_addr & ~0xFFFULL;
        for (int i = 0; i < 4096 && page_candidate >= 0x200000000ULL; ++i) {
            if (*(const uint32_t*)page_candidate == 0xFEEDFACF) {
                d3dmetal_base = page_candidate;
                break;
            }
            page_candidate -= 0x1000;
        }

        if (d3dmetal_base == 0) {
            return false;
        }
        out_d3dmetal_base = d3dmetal_base;

        // 3. Scan executable sections of D3DMetal for: 48 8d 3d [disp32] (lea rdi, [rip + disp32])
        // matching str_addr (target == instr + 7 + disp32)
        const uint8_t* lea_instr = nullptr;
        scan_ptr = d3dmetal_base;
        while (scan_ptr < d3dmetal_base + 0x600000ULL && VirtualQuery((LPCVOID)scan_ptr, &mbi, sizeof(mbi)) != 0) {
            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                !(mbi.Protect & PAGE_GUARD))
            {
                const uint8_t* start = (const uint8_t*)mbi.BaseAddress;
                const size_t size = mbi.RegionSize;
                if (size >= 7) {
                    const uint8_t* p_end = start + size - 7;
                    for (const uint8_t* p = start; p <= p_end; ++p) {
                        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x3D) {
                            const int32_t disp = *(const int32_t*)(p + 3);
                            const uintptr_t target = (uintptr_t)p + 7 + disp;
                            if (target == str_addr) {
                                lea_instr = p;
                                break;
                            }
                        }
                    }
                }
            }
            if (lea_instr != nullptr) {
                break;
            }
            scan_ptr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        }

        if (lea_instr == nullptr) {
            return false;
        }

        // 4. Resolve getenv and setenv via D3DMetal's stub/GOT
        // Right after lea (at offset +7): e8 [disp32] (callq stub_getenv)
        void* setenv_fn = nullptr;
        if (lea_instr[7] == 0xE8) {
            const int32_t call_disp = *(const int32_t*)(lea_instr + 8);
            const uint8_t* stub = lea_instr + 7 + 5 + call_disp;
            // Stub starts with: ff 25 [disp32] (jmpq *got_entry(%rip))
            if (stub[0] == 0xFF && stub[1] == 0x25) {
                const int32_t jmp_disp = *(const int32_t*)(stub + 2);
                const uintptr_t got_getenv = (uintptr_t)stub + 6 + jmp_disp;
                const void* getenv_ptr = *(const void**)got_getenv;

                if (getenv_ptr != nullptr && (uintptr_t)getenv_ptr > 0x7ff000000000ULL) {
                    // Try dlsym from GOT table (0x78 bytes before got_getenv in D3DMetal __got)
                    const uintptr_t got_dlsym = got_getenv - 0x78;
                    const void* dlsym_ptr = *(const void**)got_dlsym;
                    if (dlsym_ptr != nullptr && (uintptr_t)dlsym_ptr > 0x7ff000000000ULL) {
                        setenv_fn = call_sysv_dlsym((void*)dlsym_ptr, (void*)-2 /* RTLD_DEFAULT */, "setenv");
                    }

                    // Fallback: walk backward from getenv to libsystem_c Mach-O header
                    if (setenv_fn == nullptr) {
                        uintptr_t libsys_base = (uintptr_t)getenv_ptr & ~0xFFFULL;
                        for (int i = 0; i < 4096 && libsys_base > 0x7ff000000000ULL; ++i) {
                            if (*(const uint32_t*)libsys_base == 0xFEEDFACF) {
                                break;
                            }
                            libsys_base -= 0x1000;
                        }
                        if (*(const uint32_t*)libsys_base == 0xFEEDFACF) {
                            // On macOS x86_64, getenv is at 0xF08, setenv is at 0x536C (delta 0x4464)
                            setenv_fn = (void*)(libsys_base + 0x536C);
                        }
                    }

                    // Fallback: direct relative delta from getenv
                    if (setenv_fn == nullptr) {
                        setenv_fn = (void*)((uintptr_t)getenv_ptr + 0x4464);
                    }
                }
            }
        }

        // 5. Invoke setenv("D3DM_MTL4", "0", 0) via SysV AMD64 ABI thunk
        if (setenv_fn != nullptr) {
            out_setenv_fn = (uintptr_t)setenv_fn;
            out_setenv_res = call_sysv_setenv(setenv_fn, "D3DM_MTL4", "0", 0);
        }

        // 6. Scan forward up to 48 bytes from lea_instr for: 88 1d [disp32] (movb %bl, mtl4EV(%rip))
        uint8_t* mtl4EV_addr = nullptr;
        for (int i = 7; i < 48; ++i) {
            const uint8_t* p = lea_instr + i;
            if (p[0] == 0x88 && p[1] == 0x1D) {
                const int32_t disp = *(const int32_t*)(p + 2);
                mtl4EV_addr = (uint8_t*)(p + 6 + disp);
                break;
            }
        }

        if (mtl4EV_addr != nullptr) {
            DWORD old_prot = 0;
            if (VirtualProtect(mtl4EV_addr, 1, PAGE_READWRITE, &old_prot)) {
                out_prior_val = *mtl4EV_addr;
                *mtl4EV_addr = 0; // Force Metal 3 mode (false)
                VirtualProtect(mtl4EV_addr, 1, old_prot, &old_prot);
                out_mtl4_addr = (uintptr_t)mtl4EV_addr;
            }
        }

        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void DarwinHost::enforce_metal3_mode() {
    if (!is_darwin()) {
        return;
    }

    static bool s_enforced = false;
    if (s_enforced) {
        return;
    }

    uintptr_t d3dmetal_base = 0;
    uintptr_t setenv_fn = 0;
    int setenv_res = -1;
    uintptr_t mtl4_addr = 0;
    uint8_t prior_val = 0;

    const bool ok = enforce_metal3_mode_seh(d3dmetal_base, setenv_fn, setenv_res, mtl4_addr, prior_val);
    if (ok) {
        spdlog::info("DarwinHost: Enforced Metal 3 mode (D3DMetal base: {:p}, setenv: {:p} -> {}, mtl4EV: {:p}, prior: {})",
            (void*)d3dmetal_base, (void*)setenv_fn, setenv_res, (void*)mtl4_addr, prior_val);
        s_enforced = true;
    } else {
        spdlog::warn("DarwinHost: enforce_metal3_mode encountered exception or failed to locate D3DMetal structures");
    }
}

} // namespace utility
