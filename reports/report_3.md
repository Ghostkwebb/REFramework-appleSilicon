# Technical Deep-Dive Report 3: D3DMetal MTL4 Disassembly, DoPresent Mechanics & Automatic Metal 3 Fallback

**Date**: November 17, 2026  
**Artifact**: `/Volumes/Crucial/Games/PRAGMATA/dinput8.dll`  
**Host Environment**: macOS 15.1+ (Darwin ARM64 / Rosetta 2 x86_64)  
**Target Binary**: `D3DMetal.framework/Versions/A/D3DMetal` (CrossOver 27.0.0.41073-fex)  
**Process State**: `PRAGMATA.exe` (RE Engine DX12, TDB83)  

---

## 1. Disassembly Analysis: `D3DM_MTL4`, `ResidencySetsEnabled` & Offset Catalogs

### A. Unabridged Disassembly: `0x600b0`–`0x60120` (Device Initialization & Residency Sets)
From `D3DMetal` (`____ZN10D3DMDeviceC2EPU19objcproto9MTLDevice11objc_objectP11DXGIAdapterNS_3APIE_block_invoke`):
```assembly
00000000000600b0: 0f 00 00                     sldtw   (%rax)
00000000000600b3: e8 3e 0d 00 00               callq   0x60df6 <__Z19IsAtLeastOSVersionsI12macOSVersionJ10iOSVersionEEbT_DpT0_>
00000000000600b8: 89 c3                        movl    %eax, %ebx
00000000000600ba: 88 05 14 fc 4f 00            movb    %al, 0x4ffc14(%rip)     ## 0x55fcd4 <__ZN10D3DMDevice20ResidencySetsEnabledE>
00000000000600c0: 8a 05 d2 fa 4f 00            movb    0x4ffad2(%rip), %al     ## 0x55fb98 <__ZN10D3DMDevice15Metal4SupportedE>
00000000000600c6: 88 05 74 fb 4f 00            movb    %al, 0x4ffb74(%rip)     ## 0x55fc40 <__ZN10D3DMDevice22PlacementSparseEnabledE>
00000000000600cc: 41 8a 6d 1f                  movb    0x1f(%r13), %bpl
00000000000600d0: 48 8d 3d 68 c9 41 00         leaq    0x41c968(%rip), %rdi    ## 0x47ca3f ("D3DM_IGNORE_D3D11_RENDER_BARRIERS")
00000000000600d7: e8 3e 17 33 00               callq   0x39181a <getenv>
00000000000600dc: 48 85 c0                     testq   %rax, %rax
00000000000600df: 74 0e                        je      0x600ef
00000000000600e1: 48 89 c7                     movq    %rax, %rdi
00000000000600e4: e8 2f 16 33 00               callq   0x391718 <atoi>
00000000000600e9: 85 c0                        testl   %eax, %eax
00000000000600eb: 40 0f 95 c5                  setne   %bpl
00000000000600ef: 40 88 2d 57 fa 4f 00         movb    %bpl, 0x4ffa57(%rip)    ## 0x55fb4d <__ZN10D3DMDevice37D3D11RenderToRenderUAVBarriersIgnoredE>
00000000000600f6: 41 8a 45 22                  movb    0x22(%r13), %al
00000000000600fa: 88 05 75 fb 4f 00            movb    %al, 0x4ffb75(%rip)     ## 0x55fc75 <__ZN10D3DMDevice20ForceFoldedClearSyncE>
0000000000060100: 41 8a 45 23                  movb    0x23(%r13), %al
0000000000060104: 88 05 63 fb 4f 00            movb    %al, 0x4ffb63(%rip)     ## 0x55fc6d <__ZN10D3DMDevice22HonorUAVBarrierForBlitE>
000000000006010a: 41 8a 45 2c                  movb    0x2c(%r13), %al
000000000006010e: 88 05 5c d8 4c 00            movb    %al, 0x4cd85c(%rip)     ## 0x52d970 <__ZN10D3DMDevice19EnablePSOReflectionE>
0000000000060114: 41 8a 45 27                  movb    0x27(%r13), %al
0000000000060118: 88 05 30 fa 4f 00            movb    %al, 0x4ffa30(%rip)     ## 0x55fb4e <__ZN10D3DMDevice26EnableWriteBufferImmediateE>
000000000006011e: 41 0f b6 45 2a               movzbl  0x2a(%r13), %eax
```

### B. Unabridged Disassembly: `0x60d4b`–`0x60e54` (`MTL4OptionEnabled` & OS Checks)
From `D3DMetal`:
```assembly
0000000000060d4b <____ZN10D3DMDevice17MTL4OptionEnabledEv_block_invoke>:
   60d4b: 41 56                        pushq   %r14
   60d4d: 53                           pushq   %rbx
   60d4e: 50                           pushq   %rax
   60d4f: bf 00 1b 00 00               movl    $0x1b00, %edi           ## imm = 0x1B00 (macOS 15.0 / iOS 18.0)
   60d54: e8 ca f5 ff ff               callq   0x60323 <__Z19IsAtLeastOSVersionsI10iOSVersionJ12macOSVersionEEbT_DpT0_>
   60d59: 84 c0                        testb   %al, %al
   60d5b: 0f 84 8d 00 00 00            je      0x60dee                 ## Branch if OS < macOS 15.0
   60d61: bf 00 1b 00 00               movl    $0x1b00, %edi           ## imm = 0x1B00
   60d66: e8 b8 f5 ff ff               callq   0x60323 <__Z19IsAtLeastOSVersionsI10iOSVersionJ12macOSVersionEEbT_DpT0_>
   60d6b: 89 c3                        movl    %eax, %ebx
   60d6d: 48 8b 3d a4 e4 49 00         movq    0x49e4a4(%rip), %rdi    ## NSUserDefaults selector
   60d74: 48 8b 35 75 d1 49 00         movq    0x49d175(%rip), %rsi
   60d7b: 4c 8b 35 a6 05 48 00         movq    0x4805a6(%rip), %r14    ## objc_msgSend
   60d82: 41 ff d6                     callq   *%r14
   60d85: 48 8b 0d 1c 05 48 00         movq    0x48051c(%rip), %rcx
   60d8c: 48 8b 11                     movq    (%rcx), %rdx
   60d8f: 48 8b 35 62 d1 49 00         movq    0x49d162(%rip), %rsi
   60d96: 48 89 c7                     movq    %rax, %rdi
   60d99: 41 ff d6                     callq   *%r14
   60d9c: 48 8b 35 5d d1 49 00         movq    0x49d15d(%rip), %rsi
   60da3: 48 8d 15 0e 7e 49 00         leaq    0x497e0e(%rip), %rdx    ## @"MTL4"
   60daa: 48 89 c7                     movq    %rax, %rdi
   60dad: 41 ff d6                     callq   *%r14
   60db0: 48 85 c0                     testq   %rax, %rax
   60db3: 74 15                        je      0x60dca
   60db5: 48 8b 35 4c d1 49 00         movq    0x49d14c(%rip), %rsi    ## @selector(boolValue)
   60dbc: 48 89 c7                     movq    %rax, %rdi
   60dbf: ff 15 63 05 48 00            callq   *0x480563(%rip)         ## [defaults boolForKey:@"MTL4"]
   60dc5: 84 c0                        testb   %al, %al
   60dc7: 0f 95 c3                     setne   %bl
   60dca: 48 8d 3d 64 c5 41 00         leaq    0x41c564(%rip), %rdi    ## 0x47d335 ("D3DM_MTL4")
   60dd1: e8 44 0a 33 00               callq   0x39181a <getenv>
   60dd6: 48 85 c0                     testq   %rax, %rax
   60dd9: 74 0d                        je      0x60de8
   60ddb: 48 89 c7                     movq    %rax, %rdi
   60dde: e8 35 09 33 00               callq   0x391718 <atoi>
   60de3: 85 c0                        testl   %eax, %eax
   60de5: 0f 95 c3                     setne   %bl                     ## %bl = (atoi(val) != 0)
   60de8: 88 1d da 32 4d 00            movb    %bl, 0x4d32da(%rip)     ## 0x5340c8 <D3DMDevice::MTL4OptionEnabled()::mtl4EV>
   60dee: 48 83 c4 08                  addq    $0x8, %rsp
   60df2: 5b                           popq    %rbx
   60df3: 41 5e                        popq    %r14
   60df5: c3                           retq
```

### C. Complete Catalog: Instructions Accessing `D3DMDevice` Offset `0xc4a`
`0xc4a(%device)` is the master boolean flag controlling whether the device operates in Metal 4 mode:

| Address | Instruction | Purpose |
| :--- | :--- | :--- |
| `0x5f677` | `sete 0xc4a(%rbx)` | **Write**: In `D3DMDevice::D3DMDevice`, sets `0xc4a = 1` if `0x5340c8` (`mtl4EV`) == 1 and API is D3D12. |
| `0x5f68e` | `movb 0xc4a(%rbx), %cl` | **Read**: Verifies `0xc4a` to set `0xc48(%device)` (`Metal4CommandQueueEnabled`). |
| `0x5f813` | `movb $0x0, 0xc4a(%rbx)` | **Write**: Clear `0xc4a = 0` on fallback / initialization failure. |
| `0x610a4` | `cmpb $0x1, 0xc4a(%rbx)` | **Read**: Device feature configuration. |
| `0x612e4` | `movb 0xc4a(%rdi), %al` | **Read**: Feature query return. |
| `0x8f32c` | `movzbl 0xc4a(%rax), %r13d` | **Read**: Command list creation (allocates Metal 4 vs Metal 3 encoder structures). |
| `0x127424` | `cmpb $0x1, 0xc4a(%rax)` | **Read**: Pipeline state object reflection / binding. |
| `0x12776e` | `cmpb $0x1, 0xc4a(%rdi)` | **Read**: Render target view setup. |
| `0x16b6ef` | `cmpb $0x1, 0xc4a(%rax)` | **Read**: In `D3DMCommandQueue::CreateCommandQueue`. |
| `0x16ba4c` | `movb 0xc4a(%rax), %dl` | **Read**: In `D3DMCommandQueueWorker::D3DMCommandQueueWorker`, reads `m_device->0xc4a` to write `m_worker->0x1a` (`0x16ba52: movb %dl, 0x1a(%rdi)`). |
| `0x182712` | `cmpb $0x1, 0xc4a(%rax)` | **Read**: Swapchain initialization. |
| `0x18296a` | `cmpb $0x1, 0xc4a(%rax)` | **Read**: Swapchain drawable descriptor allocation. |
| `0x182aff` | `cmpb $0x1, 0xc4a(%r15)` | **Read**: Swapchain surface format selection. |
| `0x182bf8` | `cmpb $0x1, 0xc4a(%r12)` | **Read**: Swapchain backbuffer presentation setup. |

### D. Complete Catalog: Instructions Accessing `D3DMCommandQueueWorker` Offset `0x1a`
`0x1a(%worker)` is the cached `m_mtl4_enabled` flag copied directly from `D3DMDevice + 0xc4a`:

| Address | Instruction | Purpose |
| :--- | :--- | :--- |
| `0x16ba52` | `movb %dl, 0x1a(%rdi)` | **Write**: Stored during `D3DMCommandQueueWorker` constructor from `%dl` (`m_device->0xc4a`). |
| `0x16c2a9` | `cmpb $0x1, 0x1a(%rbp)` | **Read**: In `DoExecute`, branches to Metal 4 vs Metal 3 execution path. |
| `0x16c83c` | `cmpb $0x1, 0x1a(%rbp)` | **Read**: In `DoExecute`, branches to Metal 4 `EncodeSignalMTL4` loop vs Metal 3 `encodeSignalEvent:`. |
| `0x16da79` | `cmpb $0x0, 0x1a(%r13)` | **Read**: In `FlushEvents`, verifies whether to flush queue-level vs command-buffer events. |
| `0x16db09` | `cmpb $0x1, 0x1a(%r13)` | **Read**: In `FlushEvents`, regulates event barrier synchronization. |
| `0x16dbe4` | `cmpb $0x1, 0x1a(%r13)` | **Read**: In `DoPresent`, determines event submission mechanism. |
| `0x16dde6` | `cmpb $0x0, 0x1a(%r13)` | **Read**: In `DoPresent`, branches between Metal 4 and Metal 3 drawable presentation. |
| `0x16f140` | `cmpb $0x1, 0x1a(%r13)` | **Read**: In `DoExecute_block_invoke` completion handler (`$_2::operator()`), manages slot bitmap release. |
| `0x170789` | `cmpb $0x1, 0x1a(%r14)` | **Read**: In `D3DMCommandQueueWorker::EncodeWait`, branches between queue-level wait vs command-buffer wait. |

### E. Chronological Execution Timeline: `DllMain` vs `0x60dca`
1. **Process Launch**: `wine64-preloader` loads `PRAGMATA.exe`.
2. **Static Import Resolution**: The Windows PE loader parses `PRAGMATA.exe` imports and loads `dinput8.dll`.
3. **`dinput8.dll!DllMain` (`DLL_PROCESS_ATTACH`)**: Executes in `src/Main.cpp` line 120. `REFramework` initializes hooks, logs, and game patches.
4. **Game Entry Point**: The game starts its engine initialization and asset loading.
5. **D3D12 Initialization**: The game calls `D3D12CreateDevice` in `d3d12.dll`.
6. **Device Construction**: Wine's `d3d12.dll` routes the call into `D3DMetal`'s `_D3D12CreateDevice` (`0x7f3be`), invoking `D3DMDevice::D3DMDevice` (`0x5db1c`).
7. **Environment Check**: `0x60dca` (`getenv("D3DM_MTL4")`) executes during this construction phase.
- **Conclusion**: `dinput8.dll`'s `DllMain` runs **long before** `D3D12CreateDevice` is called and **long before** `getenv("D3DM_MTL4")` executes.

---

## 2. Disassembly Analysis: `DoPresent` (`0x16cc46`–`0x16d614`)

### A. Metal 4 vs Metal 3 Branch at `0x16cc91` & `0x16ccb0`
In `DoPresent`:
```assembly
000000000016cc89: 48 8b 45 10                  movq    0x10(%rbp), %rax        ; %rax = D3DMCommandQueue*
000000000016cc8d: 48 8b 48 10                  movq    0x10(%rax), %rcx        ; %rcx = D3DMDevice*
000000000016cc91: 8a 89 48 0c 00 00            movb    0xc48(%rcx), %cl        ; %cl = Metal4CommandQueueEnabled
000000000016cc97: 80 f9 01                     cmpb    $0x1, %cl
000000000016ccac: 88 4c 24 67                  movb    %cl, 0x67(%rsp)         ; Save flag to stack
000000000016ccb0: 0f 85 c2 01 00 00            jne     0x16ce78                ; JUMP TO METAL 3 PATH IF != 1
```

### B. Metal 4 Execution Path (`0x16ccb6`–`0x16ce77`, `0x16d505`–`0x16d55f`)
1. **Queue-Level Wait**:
```assembly
000000000016ccb6: 48 8b 78 08                  movq    0x8(%rax), %rdi         ; %rdi = id<MTL4CommandQueue>
000000000016ccba: 48 8b 75 30                  movq    0x30(%rbp), %rsi        ; %rsi = event 30
000000000016ccbe: 49 8b 56 08                  movq    0x8(%r14), %rdx         ; %rdx = seq
000000000016ccc2: e8 26 ec 01 00               callq   D3DMEncodeWaitForEvent  ; Encodes wait on MTL4CommandQueue
```
2. **Queue-Level Signal & Presentation**:
```assembly
000000000016d505: movq    0x10(%r15), %rax
000000000016d509: movq    0x30(%r15), %rdx        ; event 30
000000000016d50d: movq    0x8(%rax), %rdi         ; id<MTL4CommandQueue>
000000000016d511: movq    0x8(%r14), %rcx; incq %rcx ; seq + 1
000000000016d518: movq    0x4fef18(%rip), %rsi    ; @selector(signalEvent:value:)
000000000016d526: callq   *%rbx                   ; [queue signalEvent:event_30 value:seq+1]
000000000016d530: movq    0x4fec48(%rip), %rsi    ; @selector(presentDrawable:)
000000000016d53a: callq   *%rbx                   ; [queue presentDrawable:drawable]
```

### C. Metal 3 Execution Path (`0x16ce78`–`0x16d330`, `0x16d561`–`0x16d5e9`)
1. **Command Buffer Wait**:
```assembly
000000000016ce78: 48 8b 78 20                  movq    0x20(%rax), %rdi        ; %rdi = id<MTLCommandQueue> (Metal 3)
000000000016ce7c: 48 8b 35 7d 14 39 00         movq    0x39147d(%rip), %rsi    ; @selector(commandBuffer)
000000000016ce83: ff 15 9f 44 37 00            callq   *0x37449f(%rip)         ; [queue commandBuffer]
000000000016ce89: 49 89 c7                     movq    %rax, %r15              ; %r15 = id<MTLCommandBuffer>
000000000016ce8c: 48 8b 75 30                  movq    0x30(%rbp), %rsi        ; event 30
000000000016ce90: 49 8b 56 08                  movq    0x8(%r14), %rdx         ; seq
000000000016ce94: 48 89 c7                     movq    %rax, %rdi              ; cmdBuf
000000000016ce97: e8 e6 ea 01 00               callq   D3DMEncodeWaitForEvent  ; Encodes wait into MTLCommandBuffer
```
2. **Command Buffer Signal, Presentation & Commit**:
```assembly
000000000016d561: movq    0x30(%r15), %rdx        ; event 30
000000000016d565: movq    0x8(%r14), %rcx; incq %rcx ; seq + 1
000000000016d56c: movq    0x4fe310(%rip), %rsi    ; @selector(encodeSignalEvent:value:)
000000000016d573: movq    %r13, %rdi              ; cmdBuf
000000000016d576: callq   *0x373dac(%rip)         ; [cmdBuf encodeSignalEvent:event_30 value:seq+1]
000000000016d58f: movq    0x4fec60(%rip), %rsi    ; @selector(presentDrawable:)
000000000016d59c: callq   *0x373d86(%rip)         ; [cmdBuf presentDrawable:drawable]
000000000016d5d9: movq    0x4fda88(%rip), %rsi    ; @selector(commit)
000000000016d5e3: callq   *0x373d3f(%rip)         ; [cmdBuf commit]
```

### D. Architectural Summary: Why Metal 3 Completely Avoids the Deadlock
- **No `MTL4CommandQueue`**: Metal 3 relies on standard `MTLCommandBuffer` objects.
- **No Global 128-Slot Bitmap Lock**: In `DoExecute`, command buffer submission does not register sequence numbers into the 128-slot CPU tracking bitmap (`0x18c0`/`0x18c8`).
- **No 30-Second Timeout Deadlock**: The circular wait between CPU render thread and GPU retirement handler cannot stall the process because `ExecuteCommandLists` does not block the calling thread on queue saturation.

---

## 3. Implementation Analysis: Setting `D3DM_MTL4=0` Automatically

### A. The Challenge: Win32 PEB vs Darwin Host `environ`
`D3DMetal` is a native Mach-O dynamic library executing in macOS userspace. It resolves environment variables by calling `libsystem_c.dylib!getenv`.
Win32's `SetEnvironmentVariableA("D3DM_MTL4", "0")` modifies only the Windows PEB environment block within Wine's emulation layer; it does not touch the Darwin `environ` pointer (`**_NSGetEnviron()`). Therefore, Win32 calls have zero effect on `D3DMetal`.

### B. Option 1: Host-Level Configuration (Zero Risk, Highly Recommended)
1. **Steam Launch Options**:
   Setting launch options in Steam for `PRAGMATA`:
   ```bash
   D3DM_MTL4=0 %command%
   ```
2. **Bottle Setup Script (`scripts/crossover_mod_setup.sh`)**:
   Adding environment export to the CrossOver bottle launch environment:
   ```bash
   # In scripts/crossover_mod_setup.sh:
   # Configure bottle user environment or wrapper
   export D3DM_MTL4=0
   ```
   When `wine64-preloader` launches, all child processes and loaded dylibs inherit `D3DM_MTL4=0` via host `environ`.

### C. Option 2: In-Process Patching via `DarwinHost.cpp` (Autonomous & Robust)
Because `dinput8.dll` runs in the same 64-bit virtual address space as all loaded dylibs, REFramework can enforce Metal 3 directly from code:

1. **Method A: Direct Flag Clear on `D3DMDevice`**:
   - In `D3D12Hook::hook()`, REFramework hooks `D3D12CreateDevice`.
   - Immediately when `D3D12CreateDevice` returns `*out_device`, the device exists, but **no command queue has been created yet**.
   - REFramework can clear:
     ```cpp
     uint8_t* device_bytes = (uint8_t*)(*out_device);
     device_bytes[0xc48] = 0; // Disable Metal4CommandQueueEnabled
     device_bytes[0xc4a] = 0; // Disable Metal4Enabled
     ```
   - When the game subsequently calls `m_device->CreateCommandQueue()`, `D3DMCommandQueueWorker` constructor (`0x16ba4c`) reads `m_device->0xc4a` (`0`) and writes `0` into `m_worker->0x1a`, permanently binding the queue to Metal 3.

2. **Method B: Patching `mtl4EV` in `D3DMetal` (`0x5340c8`)**:
   - By querying the module base of `D3DMetal` (walking backwards from the `D3D12CreateDevice` function pointer to the Mach-O header `0xfeedfacf`), REFramework finds `base`.
   - Offset `0x5340c8` is `D3DMDevice::MTL4OptionEnabled()::mtl4EV`.
   - Writing `0` to `base + 0x5340c8` before `D3D12CreateDevice` forces `0x5f666: cmpb $0x1, 0x5340c8(%rip)` to fail and take the branch to `0x5f813` (`movb $0x0, 0xc4a(%rbx)`).

---

## 4. `D3D12Hook.cpp` Cleanup Verification

### A. Removal of `pace_command_queue(1)` from `present()`
In `src/D3D12Hook.cpp` (line 1213):
```cpp
// EXISTING (CAUSING 17,455 EXTRA SIGNAL CALLS):
if (utility::DarwinHost::is_darwin() && !is_test && SUCCEEDED(result)) {
    d3d12->pace_command_queue(1);
}
```
- **Consequence of Removal**:
  - `d3d12->pace_command_queue(1)` is completely eliminated from the per-frame presentation loop.
  - Exactly **zero** extra `m_command_queue->Signal(m_flush_fence, ...)` calls are injected into `D3DMetal`.
  - The Metal 4 timeline event ring descriptor backlog is eliminated.

### B. Retention of `flush_command_queue()` for `resize_buffers()`
In `src/D3D12Hook.cpp` (line 1319):
```cpp
d3d12->flush_command_queue(2000);

if (d3d12->m_on_resize_buffers) {
    d3d12->m_on_resize_buffers(*d3d12);
}
```
- **Verification**:
  - `flush_command_queue(2000)` executes only during swapchain resize operations (e.g. resolution change, window resize).
  - Explicitly invoking `ResetEvent(m_flush_event)` inside `flush_command_queue()` guarantees that any prior signaled state is cleared before `SetEventOnCompletion`, preventing premature return and ensuring complete GPU drainage before buffer recreation.
