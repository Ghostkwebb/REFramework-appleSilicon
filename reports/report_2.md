# Technical Deep-Dive Report 2: Kernel Submission Architecture, Metal 4 Pacing Exhaustion & D3DMetal Configuration

**Date**: November 17, 2026  
**Artifact**: `/Volumes/Crucial/Games/PRAGMATA/dinput8.dll`  
**Host Environment**: macOS 15.1+ (Darwin ARM64 / Rosetta 2 x86_64)  
**Target Binaries**:
- `D3DMetal.framework/Versions/A/D3DMetal` (CrossOver 27.0.0.41073-fex)
- `libd3dshared.dylib` (CrossOver 27.0.0.41073-fex)
- `IOGPU.framework` (`/System/Library/PrivateFrameworks/IOGPU.framework/IOGPU`)
- `IOGPUFamily.kext` / `kernelcache` (`com.apple.iokit.IOGPUFamily`)

---

## 1. Disassembly Analysis: Kernel Submission Architecture (`IOGPU` & `IOGPUFamily`)

### A. The End-to-End Submission Dispatch Chain
When `PRAGMATA.exe` issues GPU commands via `D3DMetal`, the submission thread `com.Metal4.SubmissionQueue` (`Thread_389493`) routes submissions through the following exact path:

```
[Userspace: Rosetta 2 x86_64 / arm64]
  com.Metal4.SubmissionQueue (libdispatch serial worker)
    -> submitCommandBuffers(void*) [IOGPU.framework]
      -> IOGPUCommandQueueSubmitCommandBuffers (0x1afc92f50) [IOGPU.framework]
        -> IOConnectTrap4(connection, 0, ...) [IOKit.framework]
          -> [Mach Trap / Syscall Gate to Darwin ARM64 Kernel]

[Kernelspace: Darwin Kernel arm64e (IOGPUFamily.kext)]
  iokit_user_client_trap
    -> IOGPUDeviceUserClient::getTargetAndTrapForIndex (0xfffffe000ac26048)
      -> Trap Index 0: IOGPUDeviceUserClient::t_submit_command_buffer (0xfffffe000ac252d4)
        -> IOGPUCommandQueue::submit_command_buffers (0xfffffe000ac57124)
          -> IOGPUCommandQueue::submit_command_buffer (0xfffffe000ac57460)
            -> IOGPUCommandQueue::submitCommandBuffer (0xfffffe000ac59f0c)
              -> IOGPUCommandDescriptor::prepare (0xfffffe000ac4dc78)
```

### B. Userspace Dispatch: `IOGPUCommandQueueSubmitCommandBuffers`
Disassembly from `IOGPU.framework` (`0x1afc92f50`):
```assembly
0x1afc92f50: hint    #27
0x1afc92f54: sub     sp, sp, #160
0x1afc92f58: stp     x26, x25, [sp, #80]
0x1afc92f5c: stp     x24, x23, [sp, #96]
0x1afc92f60: stp     x22, x21, [sp, #112]
0x1afc92f64: stp     x20, x19, [sp, #128]
0x1afc92f68: stp     x29, x30, [sp, #144]
0x1afc92f6c: add     x29, sp, #144
...
0x1afc92fec: ldp     x8, x2, [x19, #16]          ; Load connection handle & queue index
0x1afc92ff0: ldr     w0, [x8, #20]               ; Mach port / io_connect_t
0x1afc92ff4: mov     w1, #0                      ; Trap index 0 (t_submit_command_buffer)
0x1afc92ff8: mov     x3, x23                     ; Arg 2: Command buffer count
0x1afc92ffc: mov     x4, x21                     ; Arg 3: Command buffer descriptors
0x1afc93000: mov     x5, x20                     ; Arg 4: Output status pointer
0x1afc93004: bl      IOConnectTrap4              ; Direct IOKit user client fast trap
0x1afc93008: mov     x24, x0
```

### C. Kernel Entry: `IOGPUDeviceUserClient::t_submit_command_buffer`
Disassembly from `kernelcache` (`0xfffffe000ac252d4`):
```assembly
0xfffffe000ac252d4: hint    #34
0xfffffe000ac252d8: pacibsp
0xfffffe000ac252dc: sub     sp, sp, #160
0xfffffe000ac252e0: stp     x24, x23, [sp, #96]
0xfffffe000ac252e4: stp     x22, x21, [sp, #112]
0xfffffe000ac252e8: stp     x20, x19, [sp, #128]
0xfffffe000ac252ec: stp     x29, x30, [sp, #144]
0xfffffe000ac252f0: add     x29, sp, #144
0xfffffe000ac25304: mov     w19, #0xe00002c2            ; kIOReturnBadArgument
0xfffffe000ac2534c: ldr     x0, [x23, #288]             ; Load IOGPUDevice
0xfffffe000ac25350: mov     x1, x22                     ; Queue ID
0xfffffe000ac25354: bl      IOGPUDevice::retainCommandQueue
0xfffffe000ac25358: cbz     x0, error_exit
0xfffffe000ac2535c: mov     x22, x0                     ; x22 = IOGPUCommandQueue*
0xfffffe000ac2536c: mov     w2, #1                      ; Single / batch flag
0xfffffe000ac25370: mov     x3, x21                     ; Arg count
0xfffffe000ac25374: bl      IOGPUCommandQueue::submit_command_buffers
```

### D. Queue Submission Gate & Denial: `submit_command_buffers`
Disassembly from `kernelcache` (`0xfffffe000ac57124`):
```assembly
0xfffffe000ac57158: mov     w24, #0xe00002bc            ; kIOReturnNotPrivileged / Error base
0xfffffe000ac57160: ldr     x0, [x19, #1328]            ; x0 = IOGPU*
0xfffffe000ac57164: mov     x1, x19                     ; x1 = IOGPUCommandQueue*
0xfffffe000ac57168: bl      IOGPU::thread_can_submit    ; Check queue gate & error state
0xfffffe000ac5716c: cbz     w0, deny_submission         ; Branch if denied
...
deny_submission:
0xfffffe000ac571b8: add     w24, w24, #27               ; w24 = 0xe00002bc + 27 = 0xe00002d7 (kIOReturnNotResponding)
0xfffffe000ac571bc: b       exit_with_error
```

In `IOGPU::thread_can_submit` (`0xfffffe000ac55fbc`):
```assembly
0xfffffe000ac55fd0: add     x8, x1, #1480               ; Check queue gate atomic state
0xfffffe000ac55fd4: ldaprb  w8, [x8]
0xfffffe000ac55fd8: tbz     w8, #0, check_device_state  ; If bit 0 set, queue submission blocked
0xfffffe000ac55fdc: mov     w20, #0
0xfffffe000ac55fe0: b       ret_can_submit
check_device_state:
0xfffffe000ac55fe8: ldr     x0, [x0, #776]              ; Device mutex
0xfffffe000ac55fec: bl      lck_mtx_lock
0xfffffe000ac55ff0: ldr     w8, [x19, #552]             ; Device flags
0xfffffe000ac55ff4: tst     w8, #0x80                   ; Bit 7: Device hung / GPURestart active
0xfffffe000ac55ff8: cset    w20, eq                     ; Return 0 if hung
0xfffffe000ac56000: ldr     w8, [x19, #216]             ; Increment active submitter count
0xfffffe000ac56004: add     w8, w8, #1
0xfffffe000ac56008: str     w8, [x19, #216]
0xfffffe000ac56010: bl      lck_mtx_unlock
```

### E. Memory Wiring Stall & Overcommit: `IOGPUCommandDescriptor::prepare`
Disassembly from `kernelcache` (`0xfffffe000ac4dc78`):
```assembly
0xfffffe000ac4ddc4: bl      IOGPU::getCurrentWiredSysMemorySize
0xfffffe000ac4ddd4: bl      IOGPU::getMaxWiredSysMemorySize
0xfffffe000ac4de84: sub     x23, x9, x21                ; Calculate shortage: current + required - max
0xfffffe000ac4dec8: bl      IOGPUScheduler::freeToWireSysMemory
```

In `IOGPUScheduler::freeToWireSysMemory` (`0xfffffe000ac40fd0`):
```assembly
0xfffffe000ac4104c: mov     w19, #0xe00002be            ; kIOReturnNoSpace / kIOReturnOverrun (0xE00002BE)
0xfffffe000ac41068: add     x0, sp, #8
0xfffffe000ac41070: bl      IOGPUResidentMemorySet::LRUIterator::LRUIterator
lru_loop:
0xfffffe000ac41078: bl      IOGPUResidentMemorySet::LRUIterator::getNextMemory
0xfffffe000ac4107c: cbz     x0, failed_to_free          ; No more evictable pages
0xfffffe000ac4109c: mov     x0, x23
0xfffffe000ac410a0: bl      IOGPUMemory::getPrepareCount
0xfffffe000ac410a4: cbnz    w0, lru_loop                ; Non-zero prepare count -> actively wired in-flight, CANNOT evict!
0xfffffe000ac410a8: mov     x0, x23
0xfffffe000ac410ac: bl      IOGPUMemory::evict          ; Evict unpinned allocation
```

If all allocations are held in-flight by the 128 command buffers (`getPrepareCount() > 0`), `freeToWireSysMemory` fails and logs:
```
IOGPUCommandDescriptor::prepare: %s <%s> FAILED To Free, should return error: <0xE00002BE> total accelerator = 0x... max = 0x...
```

### F. Why `Thread_389493` Blocked Inside `IOConnectTrap4`
1. `Thread_389493` (`com.Metal4.SubmissionQueue`) entered `IOConnectTrap4` to submit command buffer `17456`.
2. Inside `prepare()`, the kernel attempted to wire resources but hit the hard limit with `8.48 GB` already pinned.
3. Simultaneously, the GPU executed command buffer `17455`, encountered an unrecoverable fault while reading overcommitted shared surfaces, and triggered `AGX: Channel error 2`.
4. In `retireCommandBuffer` (`0xfffffe000ac5a658`), the channel error incremented `[x19, #1088]` (`GPURestarts`). Upon reaching 2 restarts in 121 submissions, it logged `Deny submissions/ignore app[wine]` and set the device error flag (`[x19, #552] |= 0x80`).
5. As a result, `Thread_389493` was caught mid-flight inside `IOConnectTrap4`, waiting on kernel synchronization primitives associated with the dead GPU channel.

---

## 2. Root Cause Analysis: The `17,455` Extra `Signal()` Calls and Channel Error 2

### A. The Impact of `pace_command_queue(1)`
In `src/D3D12Hook.cpp`, `pace_command_queue(1)` was executed on every frame:
```cpp
m_command_queue->Signal(m_flush_fence, fence_val);
```
During foreground gameplay prior to the Cabin Training freeze, this loop ran `17,455` times, injecting `17,455` distinct `Signal()` operations into `D3DMCommandQueue`.

### B. Signal Processing in `D3DMCommandQueueWorker::DoExecute`
Disassembly of `DoExecute` in `D3DMetal`:
```assembly
0x16c838: cmpb    $1, 0x1a(%rbp)              ; Check 0x1a(%rbp) -> D3DM_MTL4
0x16c83c: jne     0x16c934                    ; If 0 (Metal 3), bypass MTL4 fence path
...
; Metal 4 Event Signal Loop:
0x16c90b: movq    64(%rsp), %rbx              ; rbx = pending signal event list head
0x16c910: movq    72(%rsp), %r12              ; r12 = pending signal event list tail
0x16c915: cmpq    %r12, %rbx
0x16c918: je      events_completed
0x16c91e: movq    16(%rbp), %rax              ; rax = Worker context
0x16c922: movq    8(%rax), %rsi               ; rsi = id<MTL4CommandQueue>
0x16c926: movq    (%rbx), %rdi                ; rdi = D3DMFence*
0x16c929: movq    16(%rbx), %rcx              ; rcx = Fence value
0x16c92d: callq   0x1619e2                    ; D3DMFence::EncodeSignalMTL4
0x16c932: addq    $24, %rbx                   ; Next 24-byte signal descriptor
0x16c936: jmp     0x16c915
```

### C. Metal 3 vs Metal 4 Behavior
| Feature | Metal 3 (`D3DM_MTL4=0`, `0x1a(%rbp) == 0`) | Metal 4 (`D3DM_MTL4=1`, `0x1a(%rbp) == 1`) |
| :--- | :--- | :--- |
| **Command Queue** | `MTLCommandQueue` | `MTL4CommandQueue` |
| **Signal Encoding** | Direct `[cmdBuf encodeSignalEvent:event value:val]` (`0x16c3b2`) | Queue-level `EncodeSignalMTL4` (`0x1619e2`) |
| **Internal Sync** | Metal command buffer dependencies | Monotonic `event 30` serialization on every submission |
| **In-Flight Queue** | Driver manages ring buffer asynchronously | Strict 128-slot CPU tracking bitmap (`0x18c0`/`0x18c8`) |
| **Residency Model** | Dynamic encoder residency (`useResource:`) | Persistent `MTLResidencySet` (keeps all memory pinned) |

### D. Why Channel Error 2 Occurred on Command Buffer `17455`
1. **Residency Set Accumulation**: Under macOS 15, `D3DMDevice::ResidencySetsEnabled` (`0x55fcd4`) is set to `1` via `IsAtLeastOSVersions(0xf00)` (`0x600b3`). All loaded resources remain resident in active residency sets.
2. **Timeline Ring Bloat**: Injecting 17,455 extra timeline fence signals created 17,455 extra queue-level synchronization events in the Metal 4 timeline ring.
3. **Asset Burst at Cabin Load**: During the Cabin Training load, hundreds of MB of geometry and textures were allocated simultaneously. Pre-wired memory reached **8.48 GB**, leaving a deficit of `remainingPages = <-82>`.
4. **Channel Error 2**: The AGX GPU hardware command processor stalled while executing descriptor `17455` due to memory bus faulting on an uncommitted residency address, generating `AGX: Channel error 2` and triggering queue shutdown.

---

## 3. Configuration Flags & Environment Variables

### A. Environment Variables in `D3DMetal`
Extracted from `D3DMetal.framework/Versions/A/D3DMetal`:

| Variable | Address / Reference | Type | Description |
| :--- | :--- | :--- | :--- |
| `D3DM_MTL4` | `0x60dca` (`0x47d335`) | Integer (`0`/`1`) | **Controls Metal 4 mode**. When set to `0`, disables `MTL4CommandQueue`, forces Metal 3 path (`0x1a(%rbp) = 0`), and bypasses the 128-slot Metal 4 tracking lock. |
| `D3DM_SUPPORT_DXR` | `0x7cc6c`, `0x7dc2a` | Integer (`0`/`1`) | Toggles DirectX Raytracing (DXR) tier support. |
| `D3DM_BOUNDS_CHECK` | `0xaa29d` | Integer (`0`/`1`) | Enables out-of-bounds safety clamping in shader bytecode translation. |
| `D3DM_MAX_FPS` | `0x12b6b2` | Integer | Hard frame-rate limiter implemented inside `D3DMetal`. |
| `D3DM_IGNORE_D3D11_RENDER_BARRIERS` | `0x600d0` (`0x47ca3f`) | Integer (`0`/`1`) | Bypasses D3D11 render-to-render UAV barriers. |
| `D3DM_FLUSH_POS_INF_TO_NAN` | `0xaa2c6` | Integer (`0`/`1`) | Shader math sanitization flag. |
| `D3DM_MIN_LOD_CLAMP` | `0xaa2ec` | Float | Clamps minimum texture sampler level of detail. |
| `D3DM_LOD_BIAS` | `0xaa315` | Float | Adds global bias to texture LOD computations. |
| `D3DM_POSITION_INVARIANCE` | `0xaa33b` | Integer (`0`/`1`) | Forces position invariance in vertex shader transformations. |
| `D3DM_SAMPLE_NAN_TO_ZERO` | `0xaa364` | Integer (`0`/`1`) | Replaces NaN texture sampler results with 0.0. |
| `D3DM_DXIL_PROCESS_DEBUG_INFORMATION` | `0xaa3a1` | Integer (`0`/`1`) | Retains or strips DXIL debug names and metadata. |
| `D3DM_FORCE_RTZ_TEXWRITE` | `0xaa3cf` | Integer (`0`/`1`) | Forces round-to-zero on texture storage writes. |
| `D3DM_SHOW_HUD_STATS` | `0x12d2bb`, `0x12e2b7` | Integer (`0`/`1`) | Enables Metal performance HUD telemetry overlay. |
| `D3DM_VENDOR_ID` | `0x1695c0`, `0x16a344` | Hex string | Spoofs GPU vendor ID reported to DX12 applications (e.g. `0x10de`). |
| `D3DM_DEVICE_ID` | `0x169622`, `0x16a3a5` | Hex string | Spoofs GPU device ID reported to DX12 applications. |
| `D3DM_DEVICE_REVISION` | `0x169655`, `0x16a3d8` | Hex string | Spoofs PCI revision ID. |
| `D3DM_DEVICE_SUBSYS` | `0x16968f`, `0x16a412` | Hex string | Spoofs subsystem ID. |
| `D3DM_DEVICE_DESCRIPTION` | `0x1696ef`, `0x16a473` | String | Spoofs GPU adapter description string (e.g. `"NVIDIA GeForce RTX 4090"`). |
| `D3DM_NVNGX_PATH` | `0x169905` | File Path | Custom path for NVIDIA NGX DLL redirection. |
| `D3DM_ENABLE_METALFX` | Text string | Integer (`0`/`1`) | Toggles MetalFX spatial/temporal upscaling integration. |
| `D3DM_NOT_IMPLEMENTED` | Text string | Integer | Logging verbosity for unimplemented D3D12 API stubs. |
| `D3DM_NO_WINDOW` | Text string | Integer (`0`/`1`) | Headless rendering mode. |
| `MTL_HUD_ENABLED` | `0x74a97`, `0x12bdae` | Integer (`0`/`1`) | Apple Metal performance overlay HUD toggle. |
| `MTL_SHADER_VALIDATION` | Text string | Integer (`0`/`1`) | Metal compiler shader validation layer. |

### B. Environment Variables in `libd3dshared.dylib`
- `D3DMETAL_FRAMEWORK_PATH`: Overrides the dylib search path to load an alternative `D3DMetal.framework`.
- `D3DM_ERROR_MODE`: Configures crash/exception trapping for D3DMetal API errors.
- `D3DM_WINE_UNIX_CALL`: Controls unix-call thunk routing between Wine and native macOS libraries.

---

## 4. Design Critique: Why Capping `ExecuteCommandLists` at `in_flight >= 48` Has a Fatal Flaw

### A. The Flaw
The proposed workaround of checking `in_flight >= 48` inside `D3D12Hook::ExecuteCommandLists` and sleeping until `in_flight - 24` completes has a fatal architectural flaw:

```
Thread A (Render Thread):
  1. Records CommandList_1 (awaits Fence X value 100 on GPU)
  2. Calls ExecuteCommandLists(CommandList_1) -> in_flight reaches 48
  3. Capping logic enters: Wait until in_flight <= 24
     *** Thread A blocks synchronously in wait() ***

  [The code that was supposed to run next on Thread A]:
  4. Calls ComputeQueue->ExecuteCommandLists(...) OR CommandQueue->Signal(Fence X, 100)
```

### B. Circular Deadlock Analysis
1. In D3D12, `ExecuteCommandLists` is strictly an **asynchronous submission API**. An engine is legally entitled to submit GPU work that depends on future fence signals that have not yet been submitted to the GPU.
2. If `ExecuteCommandLists` blocks the calling CPU thread before that thread can submit the prerequisite `Signal()` (or before a companion worker thread submits its signal), the GPU queue will stall waiting for the fence.
3. Because the GPU is waiting for the fence, **none of the in-flight command lists can retire**.
4. Because none of the command lists retire, `in_flight` never drops below the threshold.
5. The CPU thread sleeps forever in `wait()`, and the GPU waits forever on the fence.
6. The exact same deadlock occurs whether the limit is 128, 48, or 16.

### C. The Correct Pacing Invariant
- **Regulate at Presentation Boundaries**: Pacing must occur in `IDXGISwapChain::Present` where the frame recording is complete and all cross-queue dependencies for that frame have already been submitted.
- **Track CPU-Signaled Fences Only**: Pacing must wait strictly on timeline fences that the CPU has *already* encoded into the GPU queue (`m_command_queue->Signal(m_fence, current_value)`), verifying that `GPU_Value >= current_value - max_frames`.
- **Do Not Restrict Asset Streaming**: During level loads, games issue bursts of dozens of `ExecuteCommandLists` without calling `Present()`. Restricting submissions during loading phases breaks asset streaming engines.
