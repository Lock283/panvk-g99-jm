# CTS, three CTS-found fixes, spinning cube over WSI, texture sampling

Labels as elsewhere: **VERIFIED-HW** (measured on the G57, 3x plus a negative
control), **VERIFIED-SRC** (read in source), **INFERENCE**, **HYPOTHESIS**.

All runs below use `MESA_SHADER_CACHE_DISABLE=true`. See
[the shader-cache pitfall](#shader-cache-pitfall) for why.

## 1. Running VK-GL-CTS on the device

`deqp-vk` from VK-GL-CTS `31807ad`, built natively in Termux as an Android
executable: `DEQP_TARGET=android`, `DEQP_ANDROID_EXE=ON`, explicit
`DE_OS=DE_OS_ANDROID DE_CPU=DE_CPU_ARM_64 DE_ANDROID_API=24`, and
`SELECTED_BUILD_TARGETS=deqp-vk`. Log, GLES and mediandk libraries come from
`/system/lib64`. Two local CTS changes were needed to build. Neither changes a
test:

| file | why |
|---|---|
| [`tests/cts/cts-local-memfd.patch`](../tests/cts/cts-local-memfd.patch) | `memfd_create` is not declared below API 30, so it is called through `syscall()` |
| [`tests/cts/cts-local-headless-cmake.patch`](../tests/cts/cts-local-headless-cmake.patch) + [`tcuAndroidHeadlessExePlatform.cpp`](../tests/cts/tcuAndroidHeadlessExePlatform.cpp) | the Android executable build has no `createPlatform()`. This one provides a Vulkan-only platform with no window system. |

There is no Vulkan loader on this path. [`vkshim.c`](../tests/cts/vkshim.c)
exports `vkGetInstanceProcAddr` and forwards to the ICD's
`vk_icdGetInstanceProcAddr`. Use it with
`--deqp-vk-library-path=libvkshim.so`. [`run_cts.py`](../tests/cts/run_cts.py)
runs a caselist in one process and resumes after a crash or hang (a hang is
60 s without progress). [`dashboard.py`](../tests/cts/dashboard.py) shows
progress and the newest rendered image on Termux:X11.

Results on the final binary (patches up to 0043), in
[`evidence/cts/`](../evidence/cts/):

| group | Pass | Fail | NotSupported |
|---|---|---|---|
| `api.smoke` | 8 | 0 | 0 |
| `draw.renderpass.simple_draw` | 4 | 0 | 0 |
| `draw.renderpass.indirect_draw` | 86 | 0 | 286 |

*(SUPERSEDED for `indirect_draw`: with the 🧪 EXPERIMENTAL patch `0044` it is 372/0/0, see [v9-experimental-features.md](v9-experimental-features.md). The 86/286 result is kept in `evidence/cts/indirect_draw_before_0044/`.)*

`indirect_draw` ran on the binary just before the 0042/0043 comment rewording.
The code was the same, and the smaller groups were rerun after the rebuild.
This is three groups out of a mustpass of about 3.2 million cases. It says
nothing about conformance.

Before the fixes below, the first CTS runs hung on every case that waits on a
fence, and `api.smoke` was 6/8.

## 2. Fixes found by CTS

None of these showed up in this project's own harnesses. Each harness did one
rendering submission per process and waited with `vkQueueWaitIdle`.

### 0041: fences and semaphores were never signalled (VERIFIED-HW)

`jm/panvk_vX_gpu_queue.c` handled waits and signals as DRM syncobjs. On a
kbase fd those ioctls fail, and because the type asserts are compiled out
(`b_ndebug=true`) the failure was silent. `vkWaitForFences` timed out even
though the jobs had completed (`DONE` in the job status). Every v9 submission
is already synchronous, so waits are now honoured with `vk_sync_wait` before
submitting, and signals are raised with `vk_sync_signal` afterwards.

[`fence_test.c`](../tests/graphics/fence_test.c), `FENCE_MODE=nocb|empty|work`:
`vkWaitForFences` returns `VK_SUCCESS` at once in all 3 modes, 3x each.
`PANVK_KBASE_NO_SIGNAL=1` gives `VK_TIMEOUT` after 3 s in all of them.
Logs: `evidence/logs/T4.8.1_*`.

### 0042: a vertex-shader gl_Layer write broke meta rectangles (VERIFIED-HW, mechanism not established)

On v9 the generic `vk_meta_draw_rects` is used (panvk's own override is
`PAN_ARCH >= 10`, VERIFIED-SRC). Its vertex shader writes `gl_Layer`. Panvk ran
Bifrost's `lower_layer_writes` only for `PAN_ARCH < 9`, so on v9 the layer
write reached the hardware. The result was that buffer→OPTIMAL image copies
lost a triangular wedge along one edge: 693 of 4096 texels were never written.
The wedge was part of the second triangle of the rectangle. The OPTIMAL (AFBC)
copy goes through the graphics path, which is why linear and compute-path
copies passed. The fix runs the same software lowering on v9, with a v9-only
`layer_id` sysval appended at the end of `panvk_graphics_sysvals` and set to 0,
because the v9 path encodes only layer 0. Why the hardware layer slot breaks
this path is **not** established.

[`copy_test.c`](../tests/graphics/copy_test.c) `COPY_CASE=rt_opt`: 0 bad, 3x.
`PANVK_V9_HW_LAYER=1`: 693 bad, 3x. This control acts at shader compile time,
so it needs the shader cache disabled. Images:
[before](../evidence/framebuffer/phase4.8/copy_rt_opt_before_0042.png),
[after](../evidence/framebuffer/phase4.8/copy_rt_opt_after_0042.png).
Logs: `evidence/logs/T4.8.2_*`.

### 0043: MALLOC_VERTEX job not zeroed (VERIFIED-HW)

`api.smoke.transfer` passed when run alone and failed when it ran after
`api.smoke.asm_triangle` in the same process. Comparing the two job dumps
showed that bytes +64..+111 and +320..+383 of the faulting MALLOC_VERTEX job
held `0xffbf4020`, which is the earlier test's clear colour. The job ended
with `DATA_INVALID_FAULT` (0x58). The job is now `memset` to 0 after
allocation. Which descriptor fields those bytes belong to was not established.

CTS caselist `asm_triangle` + `transfer` in one process: Pass/Pass 3x.
`PANVK_MVJ_NO_ZERO=1`: Pass/Fail 3x. Logs: `evidence/logs/T4.8.3_*`.

### Shader-cache pitfall

Mesa's on-disk shader cache can return a binary compiled before a
compile-time control was set. So an A/B test of a compile-time change (like
`PANVK_V9_HW_LAYER`) can look like it passes without testing anything. All
controls before 0042 act at command-recording or submit time, so they are not
affected. All runs from here on disable the cache.

## 3. Spinning cube

### Offscreen, checked per frame (VERIFIED-HW)

[`cube_test.c`](../tests/graphics/cube_test.c): a cube with one colour per
face. The MVP comes from a push constant, and position and colour come from an
interleaved vertex buffer. Depth is LESS on D32 with no culling, so hidden
faces are removed by the depth test alone. Each frame re-records the command
buffer and waits on a fence. Every frame is compared with a CPU rasterizer of
the same triangles and matrix. Pixels within 0.5 px of an edge between
different colours are counted separately. Every other pixel must match within
2 per channel.

- 36 frames, OPTIMAL (AFBC, read back by copy) and LINEAR, 3x each: 0 bad
  pixels, and per-frame results identical across runs. Against an exact
  point-in-triangle model, 150 pixels differ over 36 frames at 512×512
  (sub-pixel rounding on edges).
- `CUBE_LIE=1` (model angle +3°): 36/36 frames fail.
  `CUBE_NODEPTH=1` (depth test off in the pipeline): 34/36 frames fail.
- 3600 frames at 1000×1000, sampled every 60th: 0 bad.

Frames: [`evidence/framebuffer/phase4.9/`](../evidence/framebuffer/phase4.9/).
Logs: `evidence/logs/T4.9.1_*`, `T4.9.2_*`.

### Presented through VK_KHR_swapchain on Termux:X11 (VERIFIED-HW)

[`wsi_cube.c`](../tests/graphics/wsi_cube.c) uses `VK_KHR_xcb_surface` and a
swapchain. Each frame does `vkAcquireNextImageKHR`, renders, then
`vkQueuePresentKHR`. The program never copies pixels to the window. The
driver's WSI does that. On kbase without dma-buf, `panvk_wsi.c` selects
Mesa's software WSI path (`sw_device`, VERIFIED-SRC).

The driver needs a build with `-Dplatforms=x11`. The same tree and options
work, plus `-Dc_link_args=-landroid-shmem`, because the X11 WSI's MIT-SHM path
uses SysV shm. Meson also has to be pointed at Termux's clang explicitly
inside the proot. No driver code changes.

Every 30th frame the window is read back from the X server with
`xcb_get_image` and compared with the CPU model of that frame, so the check
covers what is on screen.

- FIFO, 600 frames, 3x: 20 frames checked per run, 0 bad, 49-56 fps.
  IMMEDIATE: 0 bad.
- `CUBE_LIE=1`: 10/10 checked frames fail.
- The software path presents asynchronously. Read back with no delay, the
  very first frame was not on screen yet (`CUBE_SETTLE_MS=0`, frame 0 fails).
  A 100 ms wait plus an X round trip is used. This is a property of the test,
  not of the frames.

Surface reported: formats 50 and 44 (`B8G8R8A8_SRGB`, `B8G8R8A8_UNORM`),
present modes 0-3, minImageCount 3, 4 images created. Logs:
`evidence/logs/T4.9.3_*.filtered.log` (driver debug lines removed, otherwise
as produced).

## 4. Texture sampling (Phase 5a, VERIFIED-HW)

[`tex_test.c`](../tests/graphics/tex_test.c) uploads an 8×8 RGBA8 pattern with
`vkCmdCopyBufferToImage`, to an OPTIMAL (AFBC) or LINEAR image. A fragment
shader samples it at `uv = gl_FragCoord.xy * scale + off`, with no varyings,
onto a 64×64 target. A CPU model of the spec filtering equations covers
nearest and linear filtering with REPEAT, MIRRORED_REPEAT, CLAMP_TO_EDGE and
CLAMP_TO_BORDER (opaque black).

8 cases × 2 tilings × 3 runs = 48/48 pass with max channel error 0 and no
clear-colour pixels. `TEX_LIE=1` (model uses the other filter, or for nearest
cases the other address mode) fails all 8 cases, with 2800-4096 of 4096
pixels wrong. Images: [`evidence/framebuffer/phase5/`](../evidence/framebuffer/phase5/).
Logs: `evidence/logs/T5.1_*`.

Not covered: mipmapping and LOD selection, anisotropy, compare samplers,
other formats, arrays, cube maps, 3D textures.
