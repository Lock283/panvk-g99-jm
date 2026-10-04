# Phase 7: Winlator, Wrapper and DXVK (2026-10-04)

This is the Phase 7 bring-up that the roadmap listed as "gap analysis done,
bring-up not attempted". Result: **real DXVK runs D3D11 through the same
Vulkan stack as a Winlator container**, on this G57, with rendering checked
by readback. It is not a games claim.

Labels as in the rest of the repo: VERIFIED-HW (run on this device, 3x plus a
negative control), VERIFIED-SRC (read in the source), INFERENCE, HYPOTHESIS.

Patches: `0047`-`0057` (see [`../patches/README.md`](../patches/README.md)).
Test build: `FourFectVK-G57-alpha3` (Winlator adrenotools zip).

## 1. Test stack

Winlator itself cannot be driven from Termux (no access to its data dir or
logcat), so the stack is rebuilt piece by piece from the Winlator APK
(Winlator CMOD / Ludashi, `com.ludashi.benchmark`):

| Layer | Used here |
|---|---|
| Launcher driver query | APK `libwinlator.so` / `libadrenotools.so`, loaded by `tools/winlator/adrenoload.c` |
| Vulkan ICD | APK imagefs `libvulkan_wrapper.so` (Winlator Wrapper) |
| Driver loading | adrenotools hooks from the APK, driver dir as installed by Winlator |
| Wine | Hangover wine 11.16 (FEX arm64ec for x86_64 code) |
| DXVK | DLLs bundled in the APK: 1.10.3, 1.11.1-sarek, 1.12.1-sarek, v2.3.1-stripped |
| Test programs | `tools/winlator/d3d11_tri.c` (D3D11 draw + readback), AIO-Graphics-Test 2.1.0 |

## 2. Why the first Winlator build (alpha1) failed (VERIFIED-SRC, decompiled APK)

1. **Import fails silently.** `AdrenotoolsManager.installDriver` unzips flat;
   a subfolder in the zip (alpha1 had `licenses/`) throws and the import
   aborts. The install folder is named after `meta.json` `"name"`.
2. **0 extensions.** The launcher queries the driver in the app process
   (`GPUInformation` JNI), without the container environment, so the
   `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER` gate returned no device.
3. **App crash.** `XServerDisplayActivity` does
   `getVulkanVersion().split(".")[2]` on `"Unknown"`.

## 3. Driver fixes

| Patch | Fix | Negative control |
|---|---|---|
| `0052` | Android HAL build compat (`buffer_handle_t`, gralloc casts) | build only |
| `0053` | Android build: v9 is exposed without the env gate | `PANVK_V9_REQUIRE_OPTIN=1` |
| `0054` | MediaTek gralloc: fd[0] is `anon_inode:gralloc_extra`, the dma-buf is fd[1]. New `u_gralloc_native_handle_dmabuf_fd()`. Fallback gralloc reports modifier INVALID and MTK allocates AFBC for GPU-only buffers, so driver-allocated AHBs get CPU usage (linear) and imported INVALID-modifier AHBs are only accepted when they are CPU-usable | `PANVK_GRALLOC_FD0=1`, `PANVK_AHB_NO_LINEAR_FIX=1` |
| `0055` | `SYNC_FD` import/export for the kbase CPU sync type; NULL sync-type guards in the runtime | `PANVK_KBASE_NO_SYNCFD=1` |
| `0056` | JM kbase `VkEvent` as an atomic flag | `PANVK_KBASE_EVENT_NO_SET=1` |
| `0057` | 🧪 Reject pipelines with tessellation or geometry stages on v9, and reject links with a `VK_NULL_HANDLE` library (DXVK 2.3.1 `createBasePipeline` does that after a failed library) | `PANVK_V9_ALLOW_TESS_GS=1` |

Build config: `-Dandroid-strict=false` (strict mode hid ~50 KHR extensions,
including timeline semaphores). Device extensions: 167, 158 through the
Wrapper.

`0047`-`0051` are the EXPERIMENTAL sampler work and the v9 layered-rendering
fix from earlier the same day, see
[`v9-experimental-features.md`](v9-experimental-features.md).

## 4. Verification

### Launcher and Wrapper (VERIFIED-HW)

`verify_wrapper.sh`: 28/28 as expected. Launcher query through the APK's
libadrenotools and through `libwinlator.so`, Wrapper device creation, WSI
FIFO / MAILBOX / IMMEDIATE / resize, each 3x, plus five controls (lie,
gralloc fd0, no linear fix, no sync_fd, alpha1 build).

If `libz.so.1` or `libc++_shared.so` is missing from the driver folder,
adrenotools **silently falls back to the system Mali driver** (1.3.303, 147
extensions). This driver identifies as 1.3.354 / 167 extensions / 26.2.99.

### CTS (VERIFIED-HW, 3x)

| group | Pass | Fail | NotSupported | control |
|---|---|---|---|---|
| `synchronization` sync_fd subset | 26 | 0 | 2 | `NO_SYNCFD`: 25 NotSupported |
| `api` / `synchronization` event subset | 19 | 0 | 13 | `EVENT_NO_SET`: 3 fail |
| `synchronization.basic` | 16 | 0 | 3 | |

### Real DXVK (VERIFIED-HW)

`d3d11_tri.exe`: a D3D11 window, swapchain, draw, readback check every N
frames, plus a lie control.

| DXVK | Result |
|---|---|
| 1.12.1-sarek (reports v1.12.0) | PASS 3x, D3D11 FL 11_1, ~50 fps, 1500-frame run 0 bad |
| v2.3.1-stripped | PASS 3x, D3D11 FL 10_1 |
| 1.10.3 | FAIL 3x "Requested feature level not supported" (needs `geometryShader`) |
| 1.11.1-sarek | FAIL 3x, same reason |
| control: lie | FAIL |
| control: alpha1 driver | FAIL |

### AIO-Graphics-Test 2.1.0 (VERIFIED-HW)

User report from Winlator (alpha2): D3D11, D3D10, D3D9 and OpenGL run, then
the app dies with `vkCreateGraphicsPipelines Exception 0xc0000005` after
`Unhandled intrinsic` messages from the Bifrost compiler.

Reproduced locally by clicking through the AIO shell on Termux:X11
(`tools/winlator/aio_click.sh`, DXVK 2.3.1):

| Scene | alpha3 (`0057`) | control: old driver | control: `PANVK_V9_ALLOW_TESS_GS=1` |
|---|---|---|---|
| GS Exploder | alive 3/3 | **crash** | **crash** |
| Tessellation | alive 3/3 | alive | alive |

Root cause (VERIFIED-SRC): DXVK still creates geometry/hull/domain shaders at
FL 10_x. panvk's TCS/TES lowering targets v10+ and there is no GS path, so
these shaders miscompile; when the library fails, DXVK 2.3.1 links a
`VK_NULL_HANDLE` library and the runtime dereferenced it.

The Tessellation scene renders nothing with either driver here. The diagonal
lines the user saw on the phone were not reproduced locally (HYPOTHESIS: the
miscompiled TCS/TES pipeline that `0057` now refuses).

## 5. What this does not show

- Nothing ran inside the Winlator app on our side; the app was only tested
  by the user (alpha2: D3D9/10/11 and GL scenes render).
- No real games.
- Geometry shaders, tessellation, transform feedback and depth bounds are
  missing. Scenes that need them stay empty. DXVK 1.10.3 / 1.11 need
  geometry shaders and do not start.
