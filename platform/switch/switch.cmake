# Nintendo Switch homebrew executable (included from the top-level CMakeLists
# when DIRT2_SWITCH): dirt2.elf, packaged as dirt2.nro. Primary renderer:
# DX11 -> DXVK -> NVK. DIRT2_SWITCH_DXVK=OFF retains D3D9/deko3d.
if(DIRT2_SWITCH_DXVK)
  add_executable(dirt2 platform/sdl/sdl_main.cpp platform/switch/switch_platform.cpp
    platform/switch/dxvk_checks.cpp platform/switch/dxvk_preflight.cpp
    platform/switch/dxvk_watchdog.cpp)
  target_compile_definitions(dirt2 PRIVATE DIRT2_SWITCH_DXVK=1 DIRT2_DEFAULT_DX11=1)
  target_compile_definitions(dirt2_shims PRIVATE DIRT2_SWITCH_DXVK=1)
  target_link_libraries(dirt2 PRIVATE dirt2_shims_d3d11)
else()
add_executable(dirt2 platform/sdl/sdl_main.cpp platform/switch/switch_platform.cpp
  shims/d3d9/deko/gpu.cpp shims/d3d9/deko/submission.cpp shims/d3d9/deko/resources.cpp shims/d3d9/deko/shaders.cpp
  shims/d3d9/deko/fixed_function.cpp shims/d3d9/deko/stateblock.cpp shims/d3d9/deko/device.cpp shims/d3d9/deko/d3d9.cpp)
target_compile_options(dirt2 PRIVATE -Wall -Wno-unused-parameter)
# Match the conformance runner's driver validation while the backend is being
# brought up. Otherwise invalid calls and channel errors lose their details.
option(DIRT2_DEKO_VALIDATION "Validate Switch renderer API calls" ON)
if(DIRT2_DEKO_VALIDATION)
  target_link_libraries(dirt2 PRIVATE deko3dd)
else()
  target_link_libraries(dirt2 PRIVATE deko3d)
endif()
target_link_options(dirt2 PRIVATE "LINKER:--wrap=nvGpuChannelAppendEntry" "LINKER:--wrap=nvGpuChannelKickoff")
endif()
# GNU ld reads each archive once: the runtime uses the lifted code's function
# table and the lifted code calls the runtime, so they are searched as a group.
# CMake knows RESCAN only on platforms it recognises; devkitA64's ld is GNU ld.
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
target_link_libraries(dirt2 PRIVATE dirt2_shims "$<LINK_GROUP:RESCAN,dirt2_gen,recomp_runtime>" dxvk_d3d9 SDL3::SDL3)

set(DIRT2_VERSION "0.1.0" CACHE STRING "NRO version")
if(DIRT2_SWITCH_DXVK)
  set(_nro_name "DiRT 2")
else()
  set(_nro_name "DiRT 2 DX9 fallback")
endif()
nx_generate_nacp(dirt2.nacp NAME "${_nro_name}" AUTHOR "Dirt2Recomp" VERSION "${DIRT2_VERSION}")
# The icon is the game's own dirt2.ico (game data, not committed) as the 256x256
# JPEG elf2nro takes, converted with macOS sips; libnx's default icon otherwise.
set(_nro_icon)
find_program(DIRT2_SIPS sips NO_CMAKE_FIND_ROOT_PATH)
if(DIRT2_SIPS AND EXISTS "${DIRT2_GAME_DIR}/dirt2.ico")
  execute_process(COMMAND "${DIRT2_SIPS}" -s format jpeg -z 256 256 "${DIRT2_GAME_DIR}/dirt2.ico"
    --out "${CMAKE_BINARY_DIR}/dirt2-icon.jpg" OUTPUT_QUIET ERROR_QUIET)
endif()
if(EXISTS "${CMAKE_BINARY_DIR}/dirt2-icon.jpg")
  set(_nro_icon ICON "${CMAKE_BINARY_DIR}/dirt2-icon.jpg")
endif()
nx_create_nro(dirt2 NACP dirt2.nacp ${_nro_icon})

if(DIRT2_SWITCH_DXVK)
  # Small native probe, without the recompiled game's static data/constructors
  # or custom exception handler. A CPU crash follows libnx/Atmosphere normally.
  add_executable(dirt2_dx11_probe EXCLUDE_FROM_ALL
    platform/switch/dxvk_test_main.cpp platform/switch/dxvk_checks.cpp
    platform/switch/dxvk_preflight.cpp)
  target_include_directories(dirt2_dx11_probe PRIVATE "${CMAKE_SOURCE_DIR}/runtime/include")
  target_link_libraries(dirt2_dx11_probe PRIVATE dxvk_d3d9 SDL3::SDL3 OpenAL::OpenAL)
  target_link_options(dirt2_dx11_probe PRIVATE "LINKER:--wrap=tmemCreate" "LINKER:--wrap=__libnx_aligned_alloc"
    "LINKER:--wrap=nvInitialize" "LINKER:--wrap=nvExit")
  nx_generate_nacp(dirt2_dx11_probe.nacp NAME "DiRT 2 GPU diagnostics" AUTHOR "Dirt2Recomp" VERSION "${DIRT2_VERSION}")
  nx_create_nro(dirt2_dx11_probe NACP dirt2_dx11_probe.nacp ${_nro_icon})
endif()
