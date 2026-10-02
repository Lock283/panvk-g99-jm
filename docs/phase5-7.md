# Phases 5 to 7: stencil, blending, CTS subsets, sustained WSI, DXVK gap

Continues [cts-wsi-texture.md](cts-wsi-texture.md). Same labels and the same
rules: 3x plus a negative control before VERIFIED-HW, and
`MESA_SHADER_CACHE_DISABLE=true` on every run. No driver changes were needed
for anything in this document. The binary is the one with patches up to 0043.

## 1. Stencil (Phase 5b, VERIFIED-HW)

[`stencil_test.c`](../tests/graphics/stencil_test.c) runs 12 cases:

- the eight stencil ops;
- pass and fail ops hit in the same draw;
- compare EQUAL and GREATER;
- write mask and compare mask;
- depth-fail op.

Each case clears stencil, then draws one to three rectangles with a fixed
stencil state.

The stencil buffer is read back through the stencil test itself. After the
case, 256 full-screen probe draws run with compare EQUAL, reference k and no
writes. Each probe writes colour R = k. Every pixel is hit by exactly the one
probe that matches its stencil value, so the colour image is the stencil
image. If EQUAL were ignored, the last probe would win everywhere. Every pixel
is compared exactly with a CPU model of the spec's stencil equations.

Formats D24S8, D32S8 and S8, 3 runs each: 105 pass, 0 fail. The 3 skips are
`depth_fail` on S8, which has no depth aspect. Each case produces two to four
distinct stencil values, no pixel is missed by every probe, and the three runs
are identical. `STENCIL_LIE=1` (the model swaps pass and fail ops) fails all
12 cases, with 320 to 896 pixels wrong. Logs: `evidence/logs/T5.2_*`. Images:
[`evidence/framebuffer/phase5/`](../evidence/framebuffer/phase5/).

## 2. Blending (Phase 5c, VERIFIED-HW)

[`blend_test.c`](../tests/graphics/blend_test.c) runs 13 blend states:

- alpha, additive, subtract, reverse subtract, min, max;
- multiply (DST_COLOR);
- blend constants;
- separate alpha;
- ONE_MINUS_DST_ALPHA / DST_ALPHA;
- SRC_ALPHA_SATURATE;
- write mask with blending off;
- write mask with blending on.

Rectangle B is drawn over rectangle A (blending off) and over the clear
colour, so each image covers four destination cases. Every pixel is compared
with a CPU model of the spec's blend equations, tolerance 1.

All 13 cases pass, 3 runs each, max error 1. The only error-1 pixels are in
the A-only region: 0.9 becomes 229 on the GPU and 230 in the model (round
half). That is float-to-unorm rounding, not blending. `BLEND_LIE=1` swaps the
source and destination factors. Where both factors are the same, it rotates
the op instead, and it swaps MIN and MAX. It fails all 13 cases.

The first version of the control only swapped factors, and it passed on
additive, subtract and reverse subtract, because ONE/ONE stays the same when
swapped. It was changed before the 3x runs. Logs: `evidence/logs/T5.3_*`.

Not covered: more than one attachment (independentBlend), dual-source
blending, logic ops, non-UNORM8 formats.

## 3. CTS subsets for Phase 5 (VERIFIED-HW)

Results are in [`evidence/cts/`](../evidence/cts/):

| group | Pass | Fail | NotSupported |
|---|---|---|---|
| `texture.filtering.2d.formats.r8g8b8a8_unorm` | 6 | 0 | 12 |
| `texture.mipmap.2d.basic` | 36 | 0 | 36 |
| `pipeline.monolithic.sampler.view_type.2d.format.r8g8b8a8_unorm` | 55 | 0 | 115 |
| `pipeline.monolithic.blend.format.r8g8b8a8_unorm` | 100 | 0 | 0 |
| `pipeline.monolithic.stencil.format.d24_unorm_s8_uint`, every 20th case | 209 | 0 | 0 |

The NotSupported reasons, from the CTS log:

- the `_compute` variants need a compute-only queue, and this device has one
  queue family;
- `VK_EXT_filter_cubic` is not exposed;
- `samplerFilterMinmax` is not exposed.

The mipmap cases pass, but the project's own texture test does not exercise
mip selection.

## 4. Sustained WSI and swapchain recreation (Phase 6, VERIFIED-HW)

[`wsi_cube.c`](../tests/graphics/wsi_cube.c) now creates the swapchain and
depth buffer inside the frame loop. With `CUBE_RESIZE_EVERY=n`, every n frames
it resizes the X window through the sizes 1000, 640, 880, 400, 720 and 1024,
then recreates the swapchain with `oldSwapchain` chained and destroys the old
images. `VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR` also trigger a
recreate. The window is still read back from the X server and checked against
the CPU model.

- Resize every 120 frames, 900 frames, 3x: 8 swapchains per run, 30 checked
  frames, 0 bad. `CUBE_LIE=1`: 16 of 16 checked frames fail. Logs:
  `evidence/logs/T6.1_*`.
- 30,000 frames with resize every 500, 3x: 60 swapchains per run, 120 checked
  frames, 0 bad. 75 to 81 fps on average, including the checks. Nothing
  reported out of date. RSS was sampled every 30 s and stayed between about
  20 and 54 MB in all three runs, with no upward trend and 6 to 7 open fds.
  Logs: `evidence/logs/T6.2_*`, with RSS in `T6.2_*_mem.tsv`.

Not covered: an Android native surface, MAILBOX, multiple windows, and GPU
memory accounting beyond process RSS. Kbase has no debugfs on this device.

## 5. DXVK requirement gap (Phase 7, VERIFIED-SRC + reported features)

DXVK's requirements are read from DXVK master `d30be2baea02`,
`src/dxvk/dxvk_device_info.cpp` (`getFeatureList()` entries with
`require = true`, and `checkDeviceCompatibility()`) and `dxvk_limits.h`. The
driver's values come from `dEQP-VK.info.device_features`, `device_extensions`
and `device_properties`, plus [`featq.c`](../tests/phase7/featq.c), which
queries the 1.1, 1.2, 1.3 and extension feature structs. All of them are in
[`evidence/phase7/`](../evidence/phase7/).

Vulkan 1.3: yes. Push constants: 256, which matches DXVK's
`MaxTotalPushDataSize` of 256.

DXVK requires these, and this driver does not report them, so DXVK will refuse
the device:

| missing | note |
|---|---|
| `geometryShader` | |
| `multiDrawIndirect` | |
| `multiViewport` | |
| `shaderClipDistance`, `shaderCullDistance` | |
| `textureCompressionBC` | INFERENCE: Mali has no BC hardware decode, so this would need emulation |
| `VK_EXT_robustness2` (`robustBufferAccess2`, `nullDescriptor`) | not exposed |

The other 40 required features and extensions are reported as supported.
Reported is not verified. Most of them (depthClamp, depthBiasClamp,
fillModeNonSolid, samplerAnisotropy, imageCubeArray, descriptorIndexing,
bufferDeviceAddress, timelineSemaphore, independentBlend) are not exercised by
any test here. `transformFeedback` is optional for DXVK and also missing.

In the Debian proot, `apt-cache policy` offers box64 0.3.4+dfsg-1 and wine
10.0~repack-6. That Wine is an arm64 build, so it runs arm64 Windows binaries
only. Nothing was installed or run. So the conclusion for Phase 7 is a list of
what blocks DXVK, not a DXVK bring-up. vkd3d-proton was not checked.
