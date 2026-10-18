cmake_minimum_required(VERSION 3.25)
project(Dirt2Recomp LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

set(DIRT2_GAME_DIR "${CMAKE_SOURCE_DIR}/Colin McRae DiRT 2 (v.1.1) [Steam-Rip]/Dirt 2"
    CACHE PATH "Folder containing dirt2_game.exe and the game data")

# The lifter is a host tool. When cross-compiling (iOS/tvOS/visionOS) it is
# built by a separate host preset and its output in generated/ is consumed.
option(DIRT2_BUILD_TOOLS "Build the host-side recompiler" ON)

include(FetchContent)

if(DIRT2_BUILD_TOOLS)
  add_subdirectory(tools/recomp)
endif()

# Lifter differential tests against Unicorn (fetched unless a system copy is found).
option(DIRT2_BUILD_TESTS "Build the lifter differential tests" OFF)
if(DIRT2_BUILD_TESTS AND DIRT2_BUILD_TOOLS)
  enable_testing()
  add_subdirectory(tests/lifter)
endif()

# Third-party libraries: built from pinned sources as static libraries, the
# same way on every platform (tools/scripts/build_deps.sh, build_graphics.sh).
# Each platform's inputs live in build/<component>/<platform>:
# macos, iphoneos, iphonesimulator. Nothing is taken from the host system.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(DIRT2_IOS ON)
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(DIRT2_PLATFORM iphonesimulator)
  else()
    set(DIRT2_PLATFORM iphoneos)
  endif()
elseif(APPLE)
  set(DIRT2_PLATFORM macos)
else()
  message(FATAL_ERROR "Only Apple platforms are wired up so far")
endif()
set(DIRT2_DEPS_DIR "${CMAKE_SOURCE_DIR}/build/deps/${DIRT2_PLATFORM}" CACHE PATH "build_deps.sh output prefix")
set(SDL3_DIR "${DIRT2_DEPS_DIR}/lib/cmake/SDL3")
set(OpenAL_DIR "${DIRT2_DEPS_DIR}/lib/cmake/OpenAL")
set(ENV{PKG_CONFIG_LIBDIR} "${DIRT2_DEPS_DIR}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

find_package(Threads REQUIRED)  # static OpenAL Soft links Threads::Threads
find_package(SDL3 REQUIRED CONFIG)
find_package(OpenAL REQUIRED CONFIG)
if(NOT TARGET SDL3::SDL3-static)
  message(FATAL_ERROR "SDL3 in ${DIRT2_DEPS_DIR} is not the static build: run tools/scripts/build_deps.sh ${DIRT2_PLATFORM}")
endif()

# Graphics: DXVK native d3d9 on MoltenVK, both static (build_graphics.sh).
# DXVK finds vkGetInstanceProcAddr and SDL's functions in the process.
set(DIRT2_DXVK_DIR "${CMAKE_SOURCE_DIR}/build/src/dxvk")  # patched copy (prepare_sources.sh)
set(DIRT2_DXVK_BUILD_DIR "${CMAKE_SOURCE_DIR}/build/dxvk/${DIRT2_PLATFORM}" CACHE PATH "DXVK meson build directory")
set(DIRT2_MOLTENVK_LIB_DIR "${CMAKE_SOURCE_DIR}/build/moltenvk/${DIRT2_PLATFORM}/lib" CACHE PATH "Folder with libMoltenVK.a")
add_library(dxvk_d3d9 STATIC IMPORTED)
set_target_properties(dxvk_d3d9 PROPERTIES
  IMPORTED_LOCATION "${DIRT2_DXVK_BUILD_DIR}/libdxvk_d3d9.a"
  INTERFACE_INCLUDE_DIRECTORIES "${DIRT2_DXVK_DIR}/include/native/directx;${DIRT2_DXVK_DIR}/include/native/windows")
add_library(moltenvk STATIC IMPORTED)
set_target_properties(moltenvk PROPERTIES IMPORTED_LOCATION "${DIRT2_MOLTENVK_LIB_DIR}/libMoltenVK.a")
set(_mvk_frameworks Metal QuartzCore IOSurface Foundation CoreGraphics)
if(DIRT2_IOS)
  list(APPEND _mvk_frameworks UIKit)
else()
  list(APPEND _mvk_frameworks AppKit IOKit)
endif()
foreach(fw ${_mvk_frameworks})
  set_property(TARGET moltenvk APPEND PROPERTY INTERFACE_LINK_LIBRARIES "-framework ${fw}")
endforeach()
set_property(TARGET dxvk_d3d9 APPEND PROPERTY INTERFACE_LINK_LIBRARIES moltenvk)

add_subdirectory(runtime)
add_subdirectory(shims)

# Recompiled game: built from the lifter output when it exists.
set(DIRT2_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH "Lifter output directory")
if(EXISTS "${DIRT2_GENERATED_DIR}/sources.txt")
  file(STRINGS "${DIRT2_GENERATED_DIR}/sources.txt" _gen)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DIRT2_GENERATED_DIR}/sources.txt")
  list(TRANSFORM _gen PREPEND "${DIRT2_GENERATED_DIR}/")
  add_library(dirt2_gen STATIC ${_gen})
  target_include_directories(dirt2_gen PRIVATE "${DIRT2_GENERATED_DIR}/src")
  target_link_libraries(dirt2_gen PUBLIC recomp_runtime)
  # Generated code is machine-written: silence warnings, keep -O2 even in debug.
  target_compile_options(dirt2_gen PRIVATE -w -O2 -fno-strict-aliasing)
  # x86 memory ordering for the lifted code (acquire loads / release stores).
  option(DIRT2_TSO "Emulate x86 TSO memory ordering in lifted code" ON)
  # Debug: guest write-watch hooks (RECOMP_WATCH=addr[,len] at run time).
  option(DIRT2_WATCH "Compile guest write-watch hooks into lifted code" OFF)
  if(DIRT2_WATCH)
    target_compile_definitions(dirt2_gen PRIVATE RECOMP_WATCH=1)
  endif()
  if(DIRT2_TSO)
    target_compile_definitions(dirt2_gen PRIVATE RECOMP_TSO=1)
  endif()

  if(DIRT2_IOS)
    # App bundle; signing with entitlements is done by tools/scripts/package_ios.sh.
    set(DIRT2_BUNDLE_ID "com.daniilvinogradov.dirt2recomp" CACHE STRING "iOS bundle identifier")
    add_executable(dirt2 MACOSX_BUNDLE platform/sdl/sdl_main.cpp platform/apple/text_prompt.mm platform/apple/cloud_saves.mm platform/apple/metalfx.mm)
    set_target_properties(dirt2 PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/platform/apple/Info.plist.in"
      MACOSX_BUNDLE_GUI_IDENTIFIER "${DIRT2_BUNDLE_ID}")
    # App icon from the game's own dirt2.ico (not committed: game data).
    add_custom_command(TARGET dirt2 POST_BUILD
      COMMAND "${CMAKE_SOURCE_DIR}/tools/scripts/ios_app_icon.sh" "${DIRT2_GAME_DIR}/dirt2.ico"
        $<TARGET_BUNDLE_DIR:dirt2> ${DIRT2_PLATFORM} "${CMAKE_CURRENT_BINARY_DIR}/app-icon"
      VERBATIM)
  else()
    add_executable(dirt2 platform/sdl/sdl_main.cpp platform/apple/text_prompt.mm platform/apple/cloud_saves.mm platform/apple/metalfx.mm)
  endif()
  # Looked up in the process at run time, so the linker must keep them: Vulkan's
  # entry points (DXVK, SDL) and the SDL functions DXVK's SDL3 WSI resolves
  # (build/src/dxvk/src/wsi/sdl3/wsi_platform_sdl3.cpp).
  foreach(sym vkGetInstanceProcAddr vkGetDeviceProcAddr
      SDL_GetClosestFullscreenDisplayMode SDL_GetCurrentDisplayMode SDL_GetDesktopDisplayMode SDL_GetDisplayBounds
      SDL_GetDisplayForWindow SDL_GetDisplayUsableBounds SDL_GetDisplays SDL_GetError SDL_GetFullscreenDisplayModes
      SDL_GetWindowFlags SDL_GetWindowSizeInPixels SDL_InitSubSystem SDL_IsMainThread SDL_QuitSubSystem
      SDL_RunOnMainThread SDL_SetWindowFullscreen SDL_SetWindowFullscreenMode SDL_SetWindowPosition SDL_SetWindowSize
      SDL_Vulkan_CreateSurface SDL_Vulkan_GetInstanceExtensions SDL_Vulkan_LoadLibrary SDL_free)
    target_link_options(dirt2 PRIVATE "-Wl,-u,_${sym}")
  endforeach()
  set_source_files_properties(platform/apple/text_prompt.mm platform/apple/cloud_saves.mm platform/apple/metalfx.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(dirt2 PRIVATE "-framework CloudKit" "-weak_framework MetalFX")
  target_include_directories(dirt2 PRIVATE "${DIRT2_MOLTENVK_LIB_DIR}/../include")
  target_link_libraries(dirt2 PRIVATE dirt2_shims dirt2_gen recomp_runtime dxvk_d3d9 SDL3::SDL3)
  # Community gamepad mappings, loaded from next to the executable at startup.
  add_custom_command(TARGET dirt2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL_GameControllerDB/gamecontrollerdb.txt $<TARGET_FILE_DIR:dirt2>)
  # Shipped pipeline warm-up data (game-derived, never committed): DXVK's shader
  # cache and the merged pipeline list from tools/scripts/merge_pipelines.py.
  set(DIRT2_PIPELINE_DATA_DIR "${CMAKE_SOURCE_DIR}/build/pipelines" CACHE PATH
    "Folder with dirt2.dxvk.bin, dirt2.dxvk.lut and dirt2.pipelines to ship")
  foreach(f dirt2.dxvk.bin dirt2.dxvk.lut dirt2.pipelines)
    if(EXISTS "${DIRT2_PIPELINE_DATA_DIR}/${f}")
      add_custom_command(TARGET dirt2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DIRT2_PIPELINE_DATA_DIR}/${f}" $<TARGET_FILE_DIR:dirt2>)
    endif()
  endforeach()
else()
  message(STATUS "generated/sources.txt not found: run 'recomp lift' to enable the dirt2 target")
endif()
