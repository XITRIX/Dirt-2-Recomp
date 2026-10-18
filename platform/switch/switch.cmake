# Nintendo Switch homebrew executable (included from the top-level CMakeLists
# when DIRT2_SWITCH): dirt2.elf, packaged as dirt2.nro. Renderer: D3D9 on
# deko3d (shims/d3d9/deko) behind the D3D9 bridge, with the offline shader pack
# (tools/scripts/dx9_shader_census.py -> dirt2.dkshpack, shipped next to the NRO).
add_executable(dirt2 platform/sdl/sdl_main.cpp platform/switch/switch_platform.cpp
  shims/d3d9/deko/gpu.cpp shims/d3d9/deko/resources.cpp shims/d3d9/deko/shaders.cpp
  shims/d3d9/deko/device.cpp shims/d3d9/deko/d3d9.cpp)
target_compile_options(dirt2 PRIVATE -Wall -Wno-unused-parameter)
target_link_libraries(dirt2 PRIVATE deko3d)
# GNU ld reads each archive once: the runtime uses the lifted code's function
# table and the lifted code calls the runtime, so they are searched as a group.
# CMake knows RESCAN only on platforms it recognises; devkitA64's ld is GNU ld.
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
target_link_libraries(dirt2 PRIVATE dirt2_shims "$<LINK_GROUP:RESCAN,dirt2_gen,recomp_runtime>" dxvk_d3d9 SDL3::SDL3)

set(DIRT2_VERSION "0.1.0" CACHE STRING "NRO version")
nx_generate_nacp(dirt2.nacp NAME "DiRT 2" AUTHOR "Dirt2Recomp" VERSION "${DIRT2_VERSION}")
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
