# Third-party patches

The submodules stay on unmodified upstream commits; every change we need is a
patch here, applied in place by `tools/scripts/apply_patches.sh` (idempotent,
called by the build scripts). `third_party/patches/<name>/` targets
`third_party/<name>`, or MoltenVK's fetched dependency
`third_party/MoltenVK/External/<name>`.

| Upstream | Pinned at | Patch | What it does |
|---|---|---|---|
| DXVK | v3.1.1 (`b1a1c99a`) | `dxvk/0001-apple-native-support.patch` | Darwin/iOS build (d3d9 static on Apple), Vulkan and SDL found in the process, MoltenVK-tolerated features, dummy resources for null descriptors, SDL WSI on the main thread, all missing features reported at once; also experimental `EXP*` render-state tracing |
| MoltenVK | 1.4.1 (`db445ff2`) | `MoltenVK/0001-deployment-targets.patch` | macOS 12 / iOS 16 deployment targets (Xcode 26 rejects the old ones) |
| | | `MoltenVK/0002-defer-residency-removal.patch` | Keep freed allocations in the residency set until queues drain (GPU device lost) |
| | | `MoltenVK/0003-swapchain-view-follows-drawable.patch` | Rebuild cached views of swapchain images when the drawable changes (black-frame flicker) |
| | | `MoltenVK/0004-full-sample-mask-keeps-hsr.patch` | Do not export a full sample mask from shaders (keeps hidden surface removal on Apple GPUs) |
| SPIRV-Cross | MoltenVK's pin (`adec7acb`) | `SPIRV-Cross/0001-msl-mixed-compare-images.patch` | Images sampled both plainly and with depth compare stay `texture2d`; compares emulated (green video, wrong alpha) |

To change a patch: edit the submodule tree, then regenerate the file with
`git -C <tree> diff -- <files> > third_party/patches/<name>/<patch>`. Check
that the patches still reproduce the trees from a clean checkout before
committing (each must apply to the pinned commit in order).

The submodules are marked `ignore = dirty` in `.gitmodules`: their working
trees are always patched, and the patch files are the source of truth.
