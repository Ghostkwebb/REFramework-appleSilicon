# REFramework for macOS (CrossOver / Whisky / Apple Game Porting Toolkit)

This document details the macOS and Wine compatibility architecture in this repository, design rationale, upstream synchronization guidelines, and bottle setup procedures.

---

## 1. Architectural Design & Philosophy

To ensure **100% painless upstream maintenance** when rebasing against future releases of [`praydog/REFramework`](https://github.com/praydog/REFramework), all platform-specific logic is encapsulated within a modular abstraction:

- [`shared/utility/DarwinHost.hpp`](file:///Users/ghostkwebb/Desktop/REFramework/shared/utility/DarwinHost.hpp)
- [`shared/utility/DarwinHost.cpp`](file:///Users/ghostkwebb/Desktop/REFramework/shared/utility/DarwinHost.cpp)

Upstream core files are touched only with single-line gates checking `utility::DarwinHost::is_darwin()` or `utility::DarwinHost::is_wine()`. When upstream pulls in new features, refactors, or updates to SDK bindings, git rebases will apply smoothly without conflicts.

```
                              ┌──────────────────────────────────────────────┐
                              │                 Capcom Game                  │
                              │ (MH Wilds, RE4R, RE8, SF6, DD2, DRDR, etc.)   │
                              └──────────────────────┬───────────────────────┘
                                                     │ loads
                                                     ▼
                              ┌──────────────────────────────────────────────┐
                              │            dinput8.dll (REFramework)         │
                              └──────┬───────────────────────────────┬───────┘
                                     │                               │
                      ┌──────────────┴─────────────┐   ┌─────────────┴─────────────┐
                      │    D3D12Hook (D3DMetal)    │   │     D3D11Hook (DXMT/DXVK) │
                      │ - Enumerated DXGI Adapter  │   │ - Bypass memory unhooking │
                      │ - Refcount Probe Lookup    │   │ - D3D_DRIVER_TYPE_HARDWARE│
                      │ - Zero-COM Delta Present   │   │   fallback                │
                      └──────────────┬─────────────┘   └─────────────┬─────────────┘
                                     │                               │
                                     ▼                               ▼
                      ┌────────────────────────────┐   ┌───────────────────────────┐
                      │    Wine / CrossOver Core   │   │   Wine / CrossOver Core   │
                      │       (Apple Silicon)      │   │       (Apple Silicon)     │
                      └──────────────┬─────────────┘   └─────────────┬─────────────┘
                                     │                               │
                                     ▼                               ▼
                      ┌────────────────────────────┐   ┌───────────────────────────┐
                      │  Apple D3DMetal (GPTK 2/3) │   │     DXMT / Metal D3D11    │
                      └──────────────┬─────────────┘   └─────────────┬─────────────┘
                                     │                               │
                                     └───────────────┬───────────────┘
                                                     ▼
                                      ┌─────────────────────────────┐
                                      │       macOS Metal API       │
                                      │   Apple Silicon GPU (M1-M4) │
                                      └─────────────────────────────┘
```

---

## 2. Technical Incompatibilities & Fixes

### A. D3D12 Dummy Device Crash under D3DMetal
- **Problem**: Upstream REFramework creates a dummy D3D12 device via `D3D12CreateDevice(nullptr, ...)` to inspect vtables before the game renders. Under Apple's D3DMetal translation layer, passing a null adapter triggers a segmentation fault inside Metal adapter enumeration. Furthermore, temporarily unhooking in-memory PE code causes stability crashes on Wine.
- **Solution** ([`shared/utility/DarwinHost.cpp`](file:///Users/ghostkwebb/Desktop/REFramework/shared/utility/DarwinHost.cpp)):
  1. Detects Wine host running on Darwin via `wine_get_host_version`.
  2. Bypasses in-memory byte unhooking.
  3. Enumerates the active DXGI adapter (`adapter_factory->EnumAdapters(0, &adapter)`) and passes the valid adapter pointer into `D3D12CreateDevice`.
  4. Wraps the call in a SEH trampoline (`__try/__except`) with clean exception isolation to catch translation-layer anomalies without crashing the game.
  5. Includes a dynamic swapchain recovery mechanism in `create_swapchain` if offsets were not established during initial dummy device creation.

### B. Command Queue Wrapping & Deadlocks
- **Problem**: D3DMetal wraps `ID3D12CommandQueue` in internal COM proxy objects. Upstream's pointer equality check (`candidate == command_queue`) always evaluates to `false`. Calling `QueryInterface` on these proxies within swapchain scans deadlocks worker threads.
- **Solution**:
  1. **Refcount Probing**: Holds an extra reference count on the target command queue. Iterates candidate swapchain memory and calls `AddRef()` / `Release()`. If the returned refcount equals `held + 1`, the candidate is identified as the backing queue.
  2. **Offset Fallback**: Employs known D3DMetal queue memory offset `0x4C8` as a fallback.
  3. **Zero-COM Arithmetic Delta**: In `hook()`, calculates `s_wine_cq_delta = (uintptr_t)command_queue - raw_swapchain_ptr`. In `present()`, resolves the real queue via raw pointer offset addition (`d3d12->m_command_queue = raw + s_wine_cq_delta`) requiring **zero COM invocations** during real-time frame presentation.

### C. Direct3D 11 Compatibility (DXMT & DXVK)
- **Problem**: Upstream `D3D11Hook` attempts to read `D3D11CreateDeviceAndSwapChain` from disk and overwrite in-memory bytes. On Wine, `d3d11.dll` is DXMT or DXVK, causing byte mismatches and memory write violations.
- **Solution** ([`src/D3D11Hook.cpp`](file:///Users/ghostkwebb/Desktop/REFramework/src/D3D11Hook.cpp)):
  1. Gated on `DarwinHost::is_wine()`.
  2. Skips in-memory byte patching entirely.
  3. Attempts `D3D_DRIVER_TYPE_NULL`, with an automatic fallback to `D3D_DRIVER_TYPE_HARDWARE` if the translation layer does not provide a null software rasterizer.

### D. Capcom Anti-Tamper Bypass under Wine
- **Problem**: In [`src/mods/IntegrityCheckBypass.cpp`](file:///Users/ghostkwebb/Desktop/REFramework/src/mods/IntegrityCheckBypass.cpp), REFramework copies 256 bytes of `ntdll!NtProtectVirtualMemory` to heap memory to create a "pristine" unhooked syscall stub. On genuine Windows, this is a 32-byte syscall. On Wine, `NtProtectVirtualMemory` is a full C implementation with relative calls (`call rel32`). Executing copied bytes crashes immediately due to invalid relative offsets.
- **Solution**:
  1. On Wine, skips cloning `NtProtectVirtualMemory`.
  2. Directs `virtual_protect_impl` and `virtual_protect_hook` to Wine's native `VirtualProtect`, which correctly routes to the host kernel memory subsystem.

---

## 3. macOS Native Ergonomics & UI

1. **Apple HIG Dark Theme**: When running on macOS, REFramework dynamically switches ImGui styling to an Apple-inspired Dark Slate interface:
   - Window rounding: `9.0f`, frame rounding: `6.0f`, child rounding: `6.0f`.
   - Palette: Charcoal window backgrounds (`#18181A`), system blue highlights (`#0A84FF`), and subtle border separators.
2. **Mac Keyboard Support**:
   - Standard Mac keyboards lack an `Insert` key. On macOS hosts, the default menu toggle key is automatically mapped to **`F10`** (configurable in-game).
   - In [`src/re2-imgui/imgui_impl_win32.cpp`](file:///Users/ghostkwebb/Desktop/REFramework/src/re2-imgui/imgui_impl_win32.cpp), `VK_LWIN` and `VK_RWIN` (Mac Command `⌘`) are bridged to `ImGuiMod_Ctrl`, enabling native `Cmd+C`, `Cmd+V`, `Cmd+A`, and `Cmd+Z` text editing and navigation.

---

## 4. Setup & Installation in CrossOver / Whisky

A setup script is provided in [`scripts/crossover_mod_setup.sh`](file:///Users/ghostkwebb/Desktop/REFramework/scripts/crossover_mod_setup.sh):

```bash
chmod +x scripts/crossover_mod_setup.sh
./scripts/crossover_mod_setup.sh
```

The script performs the following tasks:
1. Scans CrossOver (`~/Library/Application Support/CrossOver/Bottles`) and Whisky bottle directories for installed Capcom RE Engine titles.
2. Injects the required Wine registry override into `user.reg`:
   ```ini
   [Software\\Wine\\DllOverrides]
   "dinput8"="native,builtin"
   ```
3. Creates convenient Finder symlinks under `~/CapcomMods/<GameName>` with pre-created `reframework/autorun`, `reframework/plugins`, and `natives` directories for modding.
4. Copy your compiled `dinput8.dll` directly into the game folder.

---

## 5. RE Engine Game Compatibility Table

| Game | Backend | Status on Apple Silicon (CrossOver / Whisky) |
| :--- | :---: | :--- |
| **Monster Hunter Wilds** | D3D12 (D3DMetal) | Fully working (refcount probe + delta queue fix) |
| **Monster Hunter Rise** | D3D12 (D3DMetal) | Fully working |
| **Resident Evil 4 Remake** | D3D12 (D3DMetal) | Fully working |
| **Resident Evil Village (RE8)** | D3D12 (D3DMetal) | Fully working |
| **Resident Evil 2 Remake** | D3D11 (DXMT) / D3D12 | Fully working |
| **Resident Evil 3 Remake** | D3D11 (DXMT) / D3D12 | Fully working |
| **Resident Evil 7** | D3D11 (DXMT) / D3D12 | Fully working |
| **Devil May Cry 5** | D3D11 (DXMT) / D3D12 | Fully working |
| **Street Fighter 6** | D3D12 (D3DMetal) | Fully working |
| **Dragon's Dogma 2** | D3D12 (D3DMetal) | Fully working |
| **Kunitsu-Gami: Path of the Goddess** | D3D12 (D3DMetal) | Fully working |
| **Dead Rising Deluxe Remaster** | D3D12 (D3DMetal) | Fully working |

> **Note regarding Monster Hunter: World**: *Monster Hunter: World* runs on Capcom's legacy **MT Framework** engine rather than the RE Engine. REFramework is built exclusively for RE Engine. For modding *MH: World* on CrossOver, use [Stracker's Loader](https://www.nexusmods.com/monsterhunterworld/mods/1982).

---

## 6. How to Build `dinput8.dll`

REFramework is a 64-bit Windows dynamic link library written in modern C++23 utilizing MSVC Structured Exception Handling (`/EHa`).

### Recommended: GitHub Actions Workflow
Because REFramework uses MSVC-specific features and complex C++23 template meta-programming, building in GitHub Actions using `windows-latest` is the cleanest method:

1. Fork or push your branch to GitHub.
2. The existing CI workflow at [`.github/workflows/build-pr.yml`](file:///Users/ghostkwebb/Desktop/REFramework/.github/workflows/build-pr.yml) builds the universal DLL target automatically.
3. Download the compiled `dinput8.dll` artifact from the Actions run.

### Local MSVC Build (on Windows or Windows VM)
```cmd
git submodule update --init --recursive
cmake -B build -G "Visual Studio 17 2022" -A x64 -DREFRAMEWORK_UNIVERSAL=ON
cmake --build build --config Release
```
The output file is located at `build/bin/dinput8.dll`.
