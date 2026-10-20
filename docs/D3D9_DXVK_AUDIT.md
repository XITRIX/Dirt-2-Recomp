# Global D3D9 backend comparison with DXVK

Source review: 2026-10-20. Reference: the working local Mac DXVK tree at
`build/src/dxvk`, revision `cc92063b972e28cae662fdd9032fb15179179b5e`.
The only untracked file in that tree is its build stamp. This compares D3D9
behavior and resource management, not the similarity of API names.

The review covers the public interface, resource pools and locks, formats,
texture updates, sampling, render/depth state, shaders and constants, fixed
function processing, state blocks and COM ownership, drawing, synchronization,
queries, presentation, reset and diagnostics. It is a source-level audit, not a
claim of complete D3D9 conformance or a proof that remaining game glitches have
been diagnosed. Vulkan layouts/barriers cannot be copied directly into deko3d.

## Findings and priority

“Confirmed” means the difference is visible in the inspected implementation.
It does not mean that DiRT 2 exercises it or that it explains a captured glitch.

| Priority | Area | Finding | Status / next check |
|---|---|---|---|
| High | Shader bytecode retrieval | Both `GetFunction` implementations copied all bytecode into any non-null buffer. DXVK bounds the copy by the caller's byte capacity and leaves that capacity unchanged. | **Fixed locally**, VS and PS bounds tested under ASAN/UBSAN. No evidence this caused the graphics glitches. |
| High | GPU readback validation | `GetRenderTargetData` accepted different formats and silently cropped different dimensions. Its DMA used the source image representation with the destination pitch. DXVK rejects mismatched format/extent. | **Fixed locally** before native commands. Actual valid and invalid command paths tested. |
| Medium | Surface updates | A null destination point inherited the source rectangle's nonzero origin. DXVK defaults destination coordinates to zero. | **Fixed locally**, source subrectangle, staged pixels and explicit point tested. |
| High | Capability contracts | `CheckDeviceType` and `CheckDepthStencilMatch` return success unconditionally. `CheckDeviceFormat` ignores resource type and most query-usage bits; creation checks are separate. DXVK checks these against format/resource/device support. | **Confirmed gap**. Build a capability-versus-create/use matrix before changing advertised features; changing them can change game paths. |
| High | Texture update semantics | `AddDirtyRect` is a successful no-op, and `UpdateTexture` copies entire subresources. Mip offset is chosen from level-count difference even when top dimensions match. DXVK tracks dirty boxes and chooses top/bottom mip matching using dimensions. | **Confirmed gap**. Test dirty regions, `NO_DIRTY_UPDATE`, repeated updates and differently sized chains. Do not replace wholesale with undocumented assumptions. |
| High | Memory ownership by pool | `init_core` creates an image for ordinary SYSTEMMEM and SCRATCH textures as well as GPU pools. DXVK's `TextureUsesImage` excludes CPU-only pools. Managed unlocks also upload the entire face's mip chain each time. | **Confirmed memory/transfer overhead**. A CPU-only pool redesign needs matching changes to surface updates, texture uploads and locking; not safe as an isolated allocation toggle. |
| High | Retirement without Present | Deferred frees, transient overflow and extra command storage are reclaimed at frame retirement. Offscreen tests can finish via waits without retiring frames; the subsequent game load failed at `dkMemBlockCreate`. | **Confirmed lifetime behavior; allocation-failure causality not fully proved**. Startup-test flag removed. Add a dedicated offscreen completion/retirement lifecycle or isolate tests in another process. Never reset transient memory at every wait: a pending UP draw can already own allocations. |
| High | Descriptor capacity | `Ids::take` logs exhaustion and returns slot zero, permitting a live descriptor to be overwritten. | **Confirmed exhaustion-path defect**, not observed in current logs. Test a tiny pool with in-flight users; fail allocation or grow/retire safely instead of aliasing zero. |
| Medium | Active texture hazards | The backend copied attached textures even when unused by the selected shader. DXVK uses shader sampler masks. | **Already repaired in installed 3ee6 build**. CPU command tests pass; FPS and live artifacts still need hardware comparison. |
| Medium | Alpha state dependencies | ATOC changes and render-target changes did not invalidate native alpha-test emission. DXVK updates this dependency explicitly. | **Already repaired in installed 3ee6 build**. Regression tests fail against either old path and pass with the fixes. |
| High | Shader coverage and compilation | DXVK compiles runtime bytecode; this backend requires an offline pack. `Create*Shader` can succeed with no native shader, and draws are subsequently skipped. Original and extended pack metadata enable different alpha/fog/clipping paths. | **Confirmed architectural limitation**. Keep missing/invalid/unsupported bytecode distinguishable. Add actual material replays and masked-register shader differential tests. Do not regenerate the entire game pack from a passing subset alone. |
| Medium | sRGB, blending and formats | Native sRGB views cover selected formats. Extended shaders encode unsupported target formats in the fragment epilogue, before fixed-function blending, while POSTBLENDSRGBCONVERT is advertised. DXVK coordinates format support and attachment conversion. | **Confirmed implementation placement; required format behavior needs probes**. Test encode/blend order by format and query support. Existing Wine float-sRGB failure remains relevant. |
| Medium | Queries | Only EVENT is supported, and Issue(END) waits for the GPU. DXVK has asynchronous state and additional query types. | **Confirmed limitation/performance cost**, not automatically a correctness defect for EVENT. Unsupported queries return NOTAVAILABLE. Measure game usage before implementing occlusion/timing. |
| Medium | Presentation | Primary Present ignores source/destination/dirty rectangles. Additional swap-chain Present forwards to the primary device Present, which uses the primary back buffer. No backend handling of PresentationInterval was found although IMMEDIATE and ONE are advertised. | **Confirmed incomplete routing/parameters; interval effect needs hardware check**. Test independent swap-chain contents, rectangle behavior and timing. Platform window integration must remain Switch-specific. |
| Medium | Reset / lost device | Reset largely recreates back buffers; TestCooperativeLevel always succeeds. DXVK tracks reset/device-loss and swap-chain state. | **Confirmed scope difference**. Test retained DEFAULT resources and repeated resize/reset. Windows focus/GDI behavior is not a Switch requirement. |
| Medium | Software vertex processing / special primitives | Software VP setters/getters, ProcessVertices and patch APIs remain inherited stubs; generic capability declarations need to be checked against those omissions. | **Confirmed API gap**, game relevance unestablished. Test supported hardware path first; avoid advertising/emulating unsupported paths accidentally. |
| Low | Gamma and desktop features | SetGammaRamp is a no-op; GetGammaRamp and cursor/desktop methods are stubs. FULLSCREENGAMMA is advertised. DXVK has swap-chain gamma handling. | **Confirmed capability mismatch**, not proof of the observed luminance problem. Distinguish game tone mapping from display gamma. |
| Medium | General validation | UpdateSurface still lacks several pool/format/region validations. Vertex declarations walk until an END marker without an explicit cap; UP draw size arithmetic is not generally overflow-checked. | **Confirmed defensive-validation gaps**. Add boundary tests before broad changes. Invalid-call tests are not substitutes for valid rendering coverage. |

## Areas with substantial existing implementation

| Area | Comparison and practical limit |
|---|---|
| COM ownership / state blocks | Public references and private device bindings are separated. Typed state-block selection follows DXVK/Wine behavior; targets are deliberately excluded. Existing ownership/state-block tests are useful evidence. This audit found no new ownership defect to change. |
| Buffer streaming | DISCARD renames backing storage; NOOVERWRITE preserves it; normal busy locks wait. These follow the same dependency principles as DXVK, though DXVK uses different allocation/command machinery. Aggressive wait removal needs evidence. |
| Constants | DXVK allocates fresh uniform slices. deko uses its ordered constant-load command. Persistent buffer reuse alone is not proof of a race. Keep the new batched constant tests and test real shader combinations. |
| Fixed function | Shader algorithms are already adapted from DXVK with attribution, covering lights/materials, fog, texture stages, transforms and clip planes. ABI packing, mixed programmable/FFP use, indexed blending and uncommon point/line behavior still need broader pixel tests. Algorithm reuse does not verify the integration. |
| Sampling / formats | Address/filter/aniso/LOD/sRGB views, BC formats, depth swizzles and depth comparisons exist. BC2 and selected BC3 classes use expanded storage due hardware evidence. A correct CPU-side texture dump is not proof of correct native texture fetches. |
| Draw / MRT / depth | Vertex declarations, stream frequencies, indexed/UP draws, per-output color masks, depth/stencil and MSAA state exist. Their tests cover selected cases; renderer state dependencies and special primitives are not fully covered. |
| Synchronization | Frame fences guard ordinary recycling; render/copy transitions use native barriers. DXVK's explicit hazard machinery is a reference for dependencies, not a recipe of matching barrier enum values. The per-draw slow path also forces full state restoration, so its cleaner output cannot distinguish stale state from ordering by itself. |

## Differences deliberately not “fixed” by intuition

- DXVK's `GetDeclaration` intentionally ignores the supplied element count on
  the copy path, matching the inspected Wine test. Our similar behavior is not
  the shader `GetFunction` bug and was left unchanged.
- DXVK also implements EvictManagedResources as a successful no-op, and
  ValidateDevice normally reports one pass. These alone are not evidence of a
  Switch-only defect.
- Coarser locking and two frames in flight are not inherently wrong. Correct
  ownership and completion matter more than matching DXVK's thread structure.
- Offscreen tests can pass while the game fails to initialize afterward.
  Assertion/image results and application-lifecycle success are tracked separately.

## Evidence and validation

- Public declaration inventory: 119 Device entries including IUnknown, with
  16 inherited stubs; 17 IDirect3D9 entries, with two inherited stubs. This is
  declaration coverage, not a percentage of implemented D3D9 behavior.
- Machine-readable inventory: `build/package/switch/global-dxvk-audit-20261020/api-coverage.json`.
- Previous hardware evidence: all 46 original regressions passed and matched
  Mac reference images within comparator tolerance. Latest saved Wine target
  run has 141 passes, two failures, one partial skip from 144 Mac-passing cases.
- New local tests: `test_shader_get_function.py`, `test_update_surface_origin.py`,
  `test_readback_dimensions.py`; all execute actual implementation bodies under
  ASAN/UBSAN. Switch compilation succeeds. Hardware validation remains pending.
- These three new fixes are local; the installed Switch and normal package
  remained 3ee6 during the source audit. The later user-authorized deployment is
  recorded in the candidate manifest; that is separate from hardware verification
  of the three new API fixes. The user confirmed race barriers fixed on 3ee6.

## Recommended order for the next coverage work

1. Capability-versus-creation/use consistency and readback/update boundaries.
2. Dirty-region, mip-chain matching and CPU-only resource-pool behavior.
3. Resource retirement with no Present and descriptor exhaustion.
4. Real-material differential replays using captured shaders, geometry,
   constants and texture mips, plus generated masked RGB/alpha shader cases.
5. Independent swap chains, reset, sRGB/blend ordering and query behavior.

The useful target is more independently checked behavior and fewer hidden
fallbacks, not a larger undifferentiated pass count.
