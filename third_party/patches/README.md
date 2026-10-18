# Third-party patches

The submodules stay pristine upstream checkouts. `tools/scripts/prepare_sources.sh`
(run by the build scripts) exports each one's committed tree to
`build/src/<name>` and applies the patches there; MoltenVK's fetched
dependencies (`build/src/MoltenVK/External/<name>`) are patched the same way.
All builds read from `build/src`. A copy is rebuilt when its submodule commit
or patches change, and never while it holds edits that are not in a patch.

| Upstream | Pinned at | Patch | What it does |
|---|---|---|---|
| DXVK | v3.1.1 (`b1a1c99a`) | `dxvk/0001-apple-native-support.patch` | Darwin/iOS build (d3d9 static on Apple), Vulkan and SDL found in the process, MoltenVK-tolerated features, dummy resources for null descriptors, SDL WSI on the main thread, all missing features reported at once; also experimental `EXP*` render-state tracing |
| | | `dxvk/0002-present-hook.patch` | `dxvk_set_present_hook`: called on the submission thread before `vkQueuePresentKHR` with the back buffer and swap chain image (MetalFX upscaling, `platform/apple/metalfx.mm`) |
| | | `dxvk/0003-pipeline-warmup.patch` | Pipeline record and warm-up (below); `DXVK_SHADER_CACHE_NAME`; `DXVK_VERSION` pinned to `3.1.1-dirt2.1`; compiler workers keep the default priority on Apple; shader cache written out when idle |
| MoltenVK | 1.4.1 (`db445ff2`) | `MoltenVK/0001-deployment-targets.patch` | macOS 12 / iOS 16 deployment targets (Xcode 26 rejects the old ones) |
| | | `MoltenVK/0002-defer-residency-removal.patch` | Keep freed allocations in the residency set until queues drain (GPU device lost) |
| | | `MoltenVK/0003-swapchain-view-follows-drawable.patch` | Rebuild cached views of swapchain images when the drawable changes (black-frame flicker) |
| | | `MoltenVK/0004-full-sample-mask-keeps-hsr.patch` | Do not export a full sample mask from shaders (keeps hidden surface removal on Apple GPUs) |
| DXMT | utmapp/dxmt (`66e27f37`) | `dxmt/0001-native-hwnd-is-metal-layer.patch` | A native embedder passes the `CAMetalLayer` to present into as the swap chain's HWND (there is no Wine window) |
| | | `dxmt/0002-no-semaphore-release-without-semaphore.patch` | Skip the frame-latency semaphore the native build does not create (it logged a warning on every present) |
| | | `dxmt/0003-ios-no-curses.patch` | Link `-lcurses -lxml2` for a packaged LLVM on macOS only: the iOS SDKs have no libcurses |
| | | `dxmt/0005-metalfx-native-target.patch` | `DXMT_METALFX_SPATIAL_NATIVE=1` + `dxmt_set_spatial_upscale(enabled, w, h)`: MetalFX upscales the back buffer to the embedder's drawable size while enabled and larger, else presents it unscaled; changes apply at the next Present |
| | | `dxmt/0004-pipeline-warmup.patch` | `DXMT_PIPELINE_RECORD` / `DXMT_PIPELINE_WARMUP`: record each new pipeline; queue recorded ones on the compiler threads once all their shaders exist. `DXMT_PIPELINE_TIMING=1` logs draws that wait for a pipeline |
| SPIRV-Cross | MoltenVK's pin (`adec7acb`) | `SPIRV-Cross/0001-msl-mixed-compare-images.patch` | Images sampled both plainly and with depth compare stay `texture2d`; compares emulated (green video, wrong alpha) |

Each copy in `build/src/<name>` is a small git repo: an `upstream` commit, then one
commit per patch named after it, the last tagged `patched` (MoltenVK's dependencies
are real clones with all their patches in one `patched` commit). To change the last patch, edit
the copy, then regenerate the file against the commit before it:
`git -C build/src/<name> diff patched~1 > third_party/patches/<name>/<patch>`.
To change an earlier patch, edit its `.patch` file directly (or rebase the copy).
Delete a copy prepared before per-patch commits existed so it is prepared again.
Never edit or commit inside `third_party/`.

## DXVK pipeline warm-up (`dxvk/0003-pipeline-warmup.patch`)

MoltenVK has no graphics pipeline libraries, so DXVK 3.1.1 compiles every new
shader/state combination synchronously at its first draw (a stutter), and it has
no pipeline cache. The patch adds:

- **Shared cached shaders.** `DxvkDevice::createCachedShader` registers every shader
  it returns by name and create info, and hands out the same object on later calls.
  Pipelines match shaders by object, so a pipeline the warm-up built is the one the
  game's draw finds.
- **Recording.** `DXVK_PIPELINE_RECORD=<file>` appends each pipeline created at a draw
  (`DxvkGraphicsPipeline::getPipelineHandle`): shader names and create info plus the raw
  `DxvkGraphicsPipelineStateInfo`. Header: `D2PL`, format version, `sizeof` of the
  shader options and state, `DXVK_VERSION`. Entries carry a size and FNV-1a checksum;
  a torn last entry (crash) is dropped. Known pipelines are not recorded twice.
- **Warm-up.** `DXVK_PIPELINE_WARMUP=<file>[:<file>...]` is read when the device is
  created; a `dxvk-warmup` thread loads the shaders from the shader cache, creates the
  pipelines and queues them on DXVK's compiler workers (normal priority). Each compile
  happens outside the pipeline lock; a draw that needs it meanwhile waits for that
  compile rather than starting its own. Logged as `Pipeline warm-up: queued N` and
  `compiled N pipelines in T ms`. Pipelines whose shaders are not in the cache are skipped.
- `DXVK_PIPELINE_TIMING=1` logs every pipeline compiled at a draw with its time
  (`Pipeline compiled at draw: 12.3 ms (N so far, T ms total)`): the stutter metric.
- **Worker priority on Apple.** DXVK lowers its compiler threads with
  `pthread_setschedparam(SCHED_OTHER, priority 0)`. On Darwin the range is 15..47
  (default 31), so the workers ran at 0 and starved behind the game's threads: warm-up
  took 10 s instead of 0.1 s, and a draw waiting on a worker stalled for 27 s. They now
  keep the default priority.
- **Shader cache writes.** DXVK writes new shaders to its cache in batches of 32, so up
  to 31 were lost whenever the process exited without destroying the device (the game
  never does). The writer now also writes when its queue runs empty.
- `DXVK_SHADER_CACHE_NAME=<name>` replaces the executable-path hash in the cache file
  names, so a cache can ship with the app (`dirt2.dxvk.bin/.lut`).
- `DXVK_VERSION` is pinned instead of `git describe` (which gave `3.1.1` or `v3.1.1+`
  depending on the checkout). It validates both the shader cache and pipeline files:
  **bump it** (meson.build in this patch) whenever a patch changes shader conversion,
  and re-record.

The shader cache holds only DXVK's IR and is portable across Apple devices; pipeline
files depend on the state layout, which is the same for every arm64 build of this patch.
How the game uses this and how to produce the shipped data is in HANDOVER.md.
