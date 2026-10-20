find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_wine_out "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_wine_prepare "${PROJECT_SOURCE_DIR}/tools/scripts/prepare_wine_d3d9_tests.py")
file(GLOB _wine_upstream CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_wine_prepare}" ${_wine_upstream})
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_wine_prepare}" --out "${_wine_out}"
  RESULT_VARIABLE _wine_result)
if(NOT _wine_result EQUAL 0)
  message(FATAL_ERROR "Cannot prepare native Wine D3D9 tests")
endif()
add_executable(d3d9_wine main.cpp platform.cpp runner.c
  "${_wine_out}/visual.c" "${_wine_out}/device.c" "${_wine_out}/stateblock.c" "${_wine_out}/d3d9ex.c")
target_include_directories(d3d9_wine PRIVATE "${_wine_out}" compat upstream "${PROJECT_SOURCE_DIR}/runtime/include")
target_compile_options(d3d9_wine PRIVATE "$<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_SOURCE_DIR}/compat/platform_compat.h>"
  "$<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>" -Wno-format)
target_link_libraries(d3d9_wine PRIVATE dxvk_d3d9 SDL3::SDL3)
if(DIRT2_SWITCH)
  target_sources(d3d9_wine PRIVATE switch_support.cpp switch_fault.cpp render_probe.cpp
    "${PROJECT_SOURCE_DIR}/shims/d3d9/deko/gpu.cpp"
    "${PROJECT_SOURCE_DIR}/shims/d3d9/deko/submission.cpp"
    "${PROJECT_SOURCE_DIR}/shims/d3d9/deko/fixed_function.cpp"
    "${PROJECT_SOURCE_DIR}/shims/d3d9/deko/stateblock.cpp"
    "${PROJECT_SOURCE_DIR}/shims/d3d9/deko/device.cpp"
    "${PROJECT_SOURCE_DIR}/shims/d3d9/deko/resources.cpp"
    "${PROJECT_SOURCE_DIR}/shims/d3d9/deko/shaders.cpp"
    "${PROJECT_SOURCE_DIR}/shims/d3d9/deko/d3d9.cpp")
  # The release driver deliberately omits API validation and GPU fault details.
  # Keep the standalone conformance runner diagnostic even in a Release build;
  # this does not change which driver library the game links against.
  target_link_libraries(d3d9_wine PRIVATE deko3dd)
  target_link_options(d3d9_wine PRIVATE "LINKER:--wrap=nvGpuChannelAppendEntry" "LINKER:--wrap=nvGpuChannelKickoff")
  nx_generate_nacp(d3d9_wine.nacp NAME "D3D9 Wine Tests" AUTHOR "Wine / DiRT 2 port" VERSION "0.1.0")
  nx_create_nro(d3d9_wine NACP d3d9_wine.nacp)
else()
  foreach(sym vkGetInstanceProcAddr vkGetDeviceProcAddr
      SDL_GetClosestFullscreenDisplayMode SDL_GetCurrentDisplayMode SDL_GetDesktopDisplayMode SDL_GetDisplayBounds
      SDL_GetDisplayForWindow SDL_GetDisplayUsableBounds SDL_GetDisplays SDL_GetError SDL_GetFullscreenDisplayModes
      SDL_GetWindowFlags SDL_GetWindowSizeInPixels SDL_InitSubSystem SDL_IsMainThread SDL_QuitSubSystem
      SDL_RunOnMainThread SDL_SetWindowFullscreen SDL_SetWindowFullscreenMode SDL_SetWindowPosition SDL_SetWindowSize
      SDL_Vulkan_CreateSurface SDL_Vulkan_GetInstanceExtensions SDL_Vulkan_LoadLibrary SDL_free)
    target_link_options(d3d9_wine PRIVATE "LINKER:-u,_${sym}")
  endforeach()
endif()
