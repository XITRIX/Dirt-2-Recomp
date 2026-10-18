# D3D9 on Apple: DXVK (native, static) → MoltenVK (static) → Metal

Pinned versions that worked: DXVK 3.1.1 (`b1a1c99a`), MoltenVK 1.4.1 (`db445ff2`) and MoltenVK's
SPIRV-Cross pin. Build both as static libraries; on iOS there are no dylibs.

## DXVK patches needed

1. **Apple native support:**
   - a Darwin and iOS build, with d3d9 built static;
   - Vulkan and SDL resolved **in the process** (`vkGetInstanceProcAddr` and SDL's functions), so static linking works; keep the symbols with `-u` at link time;
   - features MoltenVK lacks (geometry shaders, cull distance, depth clip, robustness2, null descriptors) treated as optional;
   - null descriptors backed by zero-filled dummy images (one per view type) and a dummy buffer;
   - SDL window calls and surface creation marshalled to the **main thread**;
   - all missing features reported at once, rather than failing on the first.
2. **Present hook** (`dxvk_set_present_hook`): called on the submission thread just before `vkQueuePresentKHR`, with the back buffer and the acquired swap-chain image. Used for MetalFX.
3. **Pipeline warm-up** (below), plus two real DXVK problems on Apple:
   - **Compiler thread priority:** DXVK lowers its workers with `pthread_setschedparam(SCHED_OTHER, 0)`. Darwin's range is 15..47 (default 31), so they ran at 0 and starved: 10 s warm-ups and a 27 s stall on one draw. Keep the default priority.
   - **Shader cache writes:** batches of 32 lose up to 31 shaders when the process exits without destroying the device (games rarely do). Write when the queue runs empty.
   - Pin `DXVK_VERSION` (git describe varies per checkout) and add `DXVK_SHADER_CACHE_NAME` so the cache can ship.

## MoltenVK patches needed

| Patch | Bug it fixes |
|---|---|
| Deployment targets macOS 12 / iOS 16 | Xcode 26 rejects the old ones |
| Defer residency removal | GPU device lost (`kIOGPUCommandBufferCallbackErrorInvalidResource`), fullscreen taking down macOS |
| Swapchain view follows the drawable | Black frames, one in three, on some launches |
| No full sample-mask export | Keeps hidden surface removal on Apple GPUs |

## SPIRV-Cross patch: the green video and wrong alpha

- **Symptom:** Bink video rendered green, and the 3D scene was sepia and overexposed.
- **Isolation:** layer by layer, on frame 120 of a video.
  - FFmpeg as the reference; our decoder matched it at 46 dB.
  - Raw Vulkan on MoltenVK: a bit-exact upload.
  - DXVK standalone with the game's dumped shaders: correct with L8, **green with A8**.
- **Cause:** DXVK 3 emits one image variable per D3D9 sampler and uses it both for plain sampling and, behind a runtime branch, for depth compare. SPIRV-Cross saw the compare and declared it `depth2d<float>`. Sampling a `depth2d` returns red splatted to all four channels, so alpha was red on every texture: A8 read 0 (green video), and L8/X8R8G8B8 lost their forced alpha of 1.
- **Fix:** an image declared non-depth and sampled both ways stays `texture2d`, and its compares are emulated (gather plus bilinear PCF, LESS_EQUAL as in D3D9; a point compare for cube and 3D). Every other shader translates exactly as before.

## Residency: device lost

- **Evidence:** `MTL_DEBUG_LAYER=1` asserted `notifyExternalReferencesNonZeroOnDealloc` within about 4 s. Under lldb the backtrace was `DxvkSubmissionQueue::finishCmdLists → ~DxvkBuffer → vkDestroyBuffer → MVKBuffer::destroy`.
- **Cause:** on macOS 15+, MoltenVK keeps resources resident through one `MTLResidencySet` per queue, and submits command buffers with **unretained** references. `removeResidency` committed the set immediately, while other in-flight command buffers still relied on it.
- **Fix:** retain the allocation, commit an empty command buffer on every queue, and remove and release only after all of them complete.
- **Verified:** 222 s under Metal validation, clean.

## Swapchain flicker

- **Measured, not guessed:** a screen recording analysed per frame showed one presented image in three black, on 2 of 5 launches, while the back buffer read just before Present was always intact. So the loss happened after Present.
- **Cause:** DXVK blits through an sRGB view of the BGRA8 swapchain image. MoltenVK made that Metal texture view from the drawable current at first use and **cached it forever**, but CAMetalLayer hands out other drawables later.
- **Fix:** rebuild the cached view when the drawable texture changes. 10 of 10 launches were clean afterwards. Forcing vsync, synchronous submits or presenting without a command buffer did not help, which ruled out pacing.

## Pipeline warm-up (shader stutter)

- **Problem:** MoltenVK has no graphics pipeline libraries, so DXVK compiles every new shader/state pipeline synchronously at its first draw: 100–400 ms each on a cold Metal cache, 2–3 ms warm.
- **Patch:**
  - `createCachedShader` returns one object per shader name and create info, so a warmed pipeline is the one the draw finds;
  - `DXVK_PIPELINE_RECORD=<file>` appends each pipeline created at a draw: shader keys plus the raw state, with a size and FNV-1a checksum per entry; a torn tail is dropped;
  - `DXVK_PIPELINE_WARMUP=<file>[:<file>]` queues the recorded pipelines on the compiler workers at device creation, compiling outside the pipeline lock;
  - `DXVK_PIPELINE_TIMING=1` logs each draw-time compile. That is the stutter metric.
- **Shipping:** record a session, then merge the recordings with the shader cache from the same run into the bundle. At first launch, copy the shipped cache into the data folder once per app build; DXVK opens it read-write and exclusive. Each device also records its own play, so the second visit to a track doesn't stutter.
- **Results:**
  - M1 Max, cold cache: 151 draw compiles (19.1 s) dropped to 1, with 153 pipelines warmed in 4.2 s before the title screen.
  - iPad Pro M4: 153 pipelines in 340 ms.
- Bump `DXVK_VERSION` whenever a patch changes shader conversion or the state layout, and re-record.
- On macOS, set `MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION=1`.

## MetalFX upscaling (render scale below 1)

- In the present hook, export the Metal objects with `VK_EXT_metal_objects`: `vkExportMetalObjectsEXT` with `VkExportMetalTextureInfoEXT` for the back buffer and the drawable image, and `VkExportMetalCommandQueueInfoEXT` for the queue.
- Encode an `MTLFXSpatialScaler` on that same queue, so it runs before MoltenVK's present. DXVK's own bilinear blit has already filled the drawable, so a skipped frame still shows correctly.
- **Details:**
  - use `colorProcessingModePerceptual` for a gamma-encoded 8-bit frame;
  - resolve MSAA first;
  - go through an intermediate texture when the drawable's usage lacks the scaler's `outputTextureUsage`;
  - cache the scaler per input size, output size and format;
  - only upscale when the source is smaller than the drawable.
- **Support check:** `[MTLFXSpatialScalerDescriptor supportsDevice:]` under `@available(macOS 13, iOS 16)`, with MetalFX weak-linked (`-weak_framework MetalFX`). On unsupported devices the menu row stays hidden.

## Simulator limits

The iOS simulator GPU lacks `descriptorIndexing`, `shaderSampledImageArrayDynamicIndexing`, `samplerMirrorClampToEdge`, `occlusionQueryPrecise`, `computeFullSubgroups` and `subgroupSizeControl`. DXVK 3's binding model needs these, so the simulator stops at adapter selection. Test rendering on a device.

## Shader inspection

- `DXVK_SHADER_DUMP_PATH=<dir>` dumps DXVK's SPIR-V. `MVK_CONFIG_DEBUG=1 MVK_CONFIG_LOG_LEVEL=3` prints the generated MSL.
- Reproduce MoltenVK's translation offline with `spirv-cross --msl --msl-version 30000 --msl-argument-buffers --msl-argument-buffer-tier 1 --msl-device-argument-buffer 0..2`.
- `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` hangs DXVK.
- A one-instruction pixel shader assembled with the SM3 assembler probes the raw texel values DXVK returns.
