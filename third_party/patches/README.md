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
| MoltenVK | 1.4.1 (`db445ff2`) | `MoltenVK/0001-deployment-targets.patch` | macOS 12 / iOS 16 deployment targets (Xcode 26 rejects the old ones) |
| | | `MoltenVK/0002-defer-residency-removal.patch` | Keep freed allocations in the residency set until queues drain (GPU device lost) |
| | | `MoltenVK/0003-swapchain-view-follows-drawable.patch` | Rebuild cached views of swapchain images when the drawable changes (black-frame flicker) |
| | | `MoltenVK/0004-full-sample-mask-keeps-hsr.patch` | Do not export a full sample mask from shaders (keeps hidden surface removal on Apple GPUs) |
| SPIRV-Cross | MoltenVK's pin (`adec7acb`) | `SPIRV-Cross/0001-msl-mixed-compare-images.patch` | Images sampled both plainly and with depth compare stay `texture2d`; compares emulated (green video, wrong alpha) |

To change a patch: edit the copy in `build/src/<name>` (a small git repo with
`upstream` and `patched` tags), then regenerate the file with
`git -C build/src/<name> diff upstream -- <files> > third_party/patches/<name>/<patch>`
(for MoltenVK's dependencies the copy is a real clone, same tags). Never edit or
commit inside `third_party/`.
