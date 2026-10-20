# DXVK fixed-function shader sources

The three original shader files in this directory come from DXVK commit
`cc92063b972e28cae662fdd9032fb15179179b5e`, `src/d3d9/shaders/`.
Their zlib license is preserved in `LICENSE`.

`tools/scripts/build_deko_ffp_shaders.py` produces explicitly marked altered
GLSL for deko3d: fixed semantic locations, combined samplers, std140 buffers,
and UAM-compatible syntax. Generated files live in `build/shaders/deko-ffp`.
The same script also builds this project's fullscreen depth-copy utilities.
The D3D9 state conversion and ABI are in `shims/d3d9/deko/fixed_function.cpp`.
