cmake_minimum_required(VERSION 3.25)
project(Dirt2Recomp LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)
endif()

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

# The lifter is a host tool. When cross-compiling (iOS/tvOS/visionOS/Switch) it is
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
# macos, iphoneos, iphonesimulator, switch. Nothing is taken from the host system
# (on Switch, devkitPro's libnx and portlibs are the platform SDK).
if(NINTENDO_SWITCH)
  set(DIRT2_SWITCH ON)
  set(DIRT2_PLATFORM switch)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(DIRT2_IOS ON)
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(DIRT2_PLATFORM iphonesimulator)
  else()
    set(DIRT2_PLATFORM iphoneos)
  endif()
elseif(APPLE)
  set(DIRT2_PLATFORM macos)
else()
  message(FATAL_ERROR "Unsupported platform: Apple (macOS, iOS) and Nintendo Switch are wired up")
endif()
if(DIRT2_SWITCH)
  option(DIRT2_SWITCH_DXVK "Primary Switch D3D11/DXVK renderer on native NVK (OFF: D3D9/deko3d fallback)" ON)
endif()
set(_deps_platform "${DIRT2_PLATFORM}")
if(DIRT2_SWITCH_DXVK)
  set(_deps_platform "switch-nvk")
endif()
set(DIRT2_DEPS_DIR "${CMAKE_SOURCE_DIR}/build/deps/${_deps_platform}" CACHE PATH "build_deps.sh output prefix")
set(SDL3_DIR "${DIRT2_DEPS_DIR}/lib/cmake/SDL3")
set(OpenAL_DIR "${DIRT2_DEPS_DIR}/lib/cmake/OpenAL")
set(ENV{PKG_CONFIG_LIBDIR} "${DIRT2_DEPS_DIR}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
if(DIRT2_SWITCH)
  if(DIRT2_SWITCH_DXVK)
    set(DIRT2_NVK_SDK "${CMAKE_SOURCE_DIR}/build/nvk-sdk/root/opt/devkitpro" CACHE PATH "Isolated NVK SDK")
    # The shared devkitA64 toolchain resolves its own installation path. Its
    # linker specs must instead see the matching isolated libnx linker script.
    set(DEVKITPRO "${DIRT2_NVK_SDK}")
  endif()
  # devkitPro's pkg-config wrapper only sees its portlibs: use the host's
  # pkg-config on our libraries, with the portlibs after them.
  find_program(DIRT2_HOST_PKG_CONFIG pkg-config NO_CMAKE_FIND_ROOT_PATH REQUIRED)
  set(PKG_CONFIG_EXECUTABLE "${DIRT2_HOST_PKG_CONFIG}" CACHE FILEPATH "" FORCE)
  set(ENV{PKG_CONFIG_LIBDIR} "${DIRT2_DEPS_DIR}/lib/pkgconfig:${DEVKITPRO}/portlibs/switch/lib/pkgconfig")
  # libnx's switch.specs reads DEVKITPRO when linking; a build started without
  # it in the environment (the preset sets it for configure only) still links.
  set(CMAKE_C_LINKER_LAUNCHER ${CMAKE_COMMAND} -E env "DEVKITPRO=${DEVKITPRO}")
  set(CMAKE_CXX_LINKER_LAUNCHER ${CMAKE_COMMAND} -E env "DEVKITPRO=${DEVKITPRO}")
endif()

find_package(Threads REQUIRED)  # static OpenAL Soft links Threads::Threads
find_package(SDL3 REQUIRED CONFIG)
find_package(OpenAL REQUIRED CONFIG)
if(NOT TARGET SDL3::SDL3-static)
  message(FATAL_ERROR "SDL3 in ${DIRT2_DEPS_DIR} is not the static build: run tools/scripts/build_deps.sh ${DIRT2_PLATFORM}")
endif()

# Graphics: DXVK native d3d9 on MoltenVK, both static (build_graphics.sh).
# DXVK finds vkGetInstanceProcAddr and SDL's functions in the process.
set(DIRT2_DXVK_DIR "${CMAKE_SOURCE_DIR}/build/src/dxvk")  # patched copy (prepare_sources.sh)
if(DIRT2_SWITCH)
  if(DIRT2_SWITCH_DXVK)
    set(DIRT2_NVK_SDK "${CMAKE_SOURCE_DIR}/build/nvk-sdk/root/opt/devkitpro" CACHE PATH "Isolated NVK SDK")
    set(DIRT2_DXVK_DIR "${CMAKE_SOURCE_DIR}/build/src/dxvk-switch")
    set(DIRT2_DXVK_SWITCH_ARCHIVE "${CMAKE_SOURCE_DIR}/build/dxvk/switch-dx11/libdxvk_switch.a" CACHE FILEPATH "Native Switch DXVK archive")
    add_library(dxvk_d3d9 STATIC IMPORTED)
    set_target_properties(dxvk_d3d9 PROPERTIES
      IMPORTED_LOCATION "${DIRT2_DXVK_SWITCH_ARCHIVE}"
      INTERFACE_INCLUDE_DIRECTORIES "${DIRT2_DXVK_DIR}/include/native/directx;${DIRT2_DXVK_DIR}/include/native/windows")
    add_library(switch_nvk STATIC IMPORTED)
    set_target_properties(switch_nvk PROPERTIES
      IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/build/nvk-sdk/libvulkan_dirt2.a"
      INTERFACE_INCLUDE_DIRECTORIES "${DIRT2_NVK_SDK}/portlibs/switch/include"
      INTERFACE_LINK_LIBRARIES "expat;zstd;z;nx")
    # SDL's static package also requests a target/library named "vulkan".
    # Resolve it to the same private archive, never a second unrenamed -lvulkan.
    add_library(vulkan ALIAS switch_nvk)
    set_property(TARGET dxvk_d3d9 APPEND PROPERTY INTERFACE_LINK_LIBRARIES switch_nvk)
    set(DIRT2_HAVE_DXVK_D3D11 ON)
  else()
  # Switch, Gate 0 (headless boot): the D3D9 bridge compiles against DXVK's
  # headers only; platform/switch provides a Direct3DCreate9 that stops there.
  add_library(dxvk_d3d9 INTERFACE)
  target_include_directories(dxvk_d3d9 INTERFACE
    "${DIRT2_DXVK_DIR}/include/native/directx" "${DIRT2_DXVK_DIR}/include/native/windows")
  endif()
  set(DIRT2_HAVE_DXMT OFF)
else()
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

# DX11 renderer: DXMT (D3D11 -> Metal), one libdxmt-native image per platform
# (build_llvm.sh, build_dxmt.sh). LGPL, so it stays a separate dynamic library
# (an embedded framework in the app); optional until the DX11 path is the default.
set(DIRT2_DXMT_SRC_DIR "${CMAKE_SOURCE_DIR}/build/src/dxmt")  # patched copy (prepare_sources.sh)
set(DIRT2_DXMT_DIR "${CMAKE_SOURCE_DIR}/build/dxmt/${DIRT2_PLATFORM}" CACHE PATH "DXMT install prefix")
if(DIRT2_IOS)
  set(_dxmt_image "${DIRT2_DXMT_DIR}/dxmt.framework/dxmt")  # embedded in the app (Frameworks/)
else()
  set(_dxmt_image "${DIRT2_DXMT_DIR}/lib/libdxmt-native.dylib")
endif()
if(EXISTS "${_dxmt_image}")
  add_library(dxmt SHARED IMPORTED)
  set_target_properties(dxmt PROPERTIES
    IMPORTED_LOCATION "${_dxmt_image}"
    INTERFACE_INCLUDE_DIRECTORIES "${DIRT2_DXMT_SRC_DIR}/include/native/directx;${DIRT2_DXMT_SRC_DIR}/include/native/windows")
  set(DIRT2_HAVE_DXMT ON)
else()
  message(STATUS "DXMT not built for ${DIRT2_PLATFORM} (tools/scripts/build_dxmt.sh): DX11 renderer disabled")
  set(DIRT2_HAVE_DXMT OFF)
endif()
endif()  # not DIRT2_SWITCH

add_subdirectory(runtime)
add_subdirectory(shims)

option(DIRT2_BUILD_D3D9_TESTS "Build the standalone macOS D3D9 reference runner" OFF)
if(DIRT2_BUILD_D3D9_TESTS AND APPLE AND NOT DIRT2_IOS)
  add_executable(d3d9_conformance tests/d3d9/main.cpp shims/d3d9/conformance.cpp)
  target_include_directories(d3d9_conformance PRIVATE runtime/include "${DIRT2_DXVK_DIR}/src")
  target_link_libraries(d3d9_conformance PRIVATE dxvk_d3d9 SDL3::SDL3)
  # DXVK resolves Vulkan and SDL's WSI functions from the process.
  foreach(sym vkGetInstanceProcAddr vkGetDeviceProcAddr
      SDL_GetClosestFullscreenDisplayMode SDL_GetCurrentDisplayMode SDL_GetDesktopDisplayMode SDL_GetDisplayBounds
      SDL_GetDisplayForWindow SDL_GetDisplayUsableBounds SDL_GetDisplays SDL_GetError SDL_GetFullscreenDisplayModes
      SDL_GetWindowFlags SDL_GetWindowSizeInPixels SDL_InitSubSystem SDL_IsMainThread SDL_QuitSubSystem
      SDL_RunOnMainThread SDL_SetWindowFullscreen SDL_SetWindowFullscreenMode SDL_SetWindowPosition SDL_SetWindowSize
      SDL_Vulkan_CreateSurface SDL_Vulkan_GetInstanceExtensions SDL_Vulkan_LoadLibrary SDL_free)
    target_link_options(d3d9_conformance PRIVATE "LINKER:-u,_${sym}")
  endforeach()
endif()

option(DIRT2_BUILD_WINE_D3D9_TESTS "Build native upstream Wine D3D9 test runner" OFF)
if(DIRT2_BUILD_WINE_D3D9_TESTS)
  add_subdirectory(tests/d3d9_wine)
endif()

# Recompiled game: built from the lifter output when it exists.
set(DIRT2_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH "Lifter output directory")
if(EXISTS "${DIRT2_GENERATED_DIR}/sources.txt")
  file(STRINGS "${DIRT2_GENERATED_DIR}/sources.txt" _gen)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DIRT2_GENERATED_DIR}/sources.txt")
  list(TRANSFORM _gen PREPEND "${DIRT2_GENERATED_DIR}/")
  if(DIRT2_SWITCH)
    # Build only the Switch mode-list check with a360p minimum. The shared
    # generated sources and Apple builds retain the original480p check.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_resolution_patch "${CMAKE_SOURCE_DIR}/tools/scripts/switch_resolution_source.py")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${_resolution_patch}" --find "${DIRT2_GENERATED_DIR}"
      OUTPUT_VARIABLE _resolution_source OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _resolution_result)
    if(NOT _resolution_result EQUAL 0)
      message(FATAL_ERROR "Cannot locate the Switch minimum-resolution check")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_resolution_source}" "${_resolution_patch}")
    set(_resolution_output "${CMAKE_CURRENT_BINARY_DIR}/switch-generated/resolution.c")
    add_custom_command(OUTPUT "${_resolution_output}"
      COMMAND "${Python3_EXECUTABLE}" "${_resolution_patch}" --patch "${_resolution_source}" "${_resolution_output}"
      DEPENDS "${_resolution_source}" "${_resolution_patch}" VERBATIM)
    list(REMOVE_ITEM _gen "${_resolution_source}")
    list(APPEND _gen "${_resolution_output}")
  endif()
  set(DIRT2_SWITCH_GENERATED_ARCHIVE "" CACHE FILEPATH "Reuse an already built Switch lifted-code archive")
  if(DIRT2_SWITCH AND DIRT2_SWITCH_GENERATED_ARCHIVE)
    if(NOT EXISTS "${DIRT2_SWITCH_GENERATED_ARCHIVE}")
      message(FATAL_ERROR "Missing prebuilt Switch lifted-code archive")
    endif()
    add_library(dirt2_gen STATIC IMPORTED)
    set_target_properties(dirt2_gen PROPERTIES IMPORTED_LOCATION "${DIRT2_SWITCH_GENERATED_ARCHIVE}")
  else()
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
  endif()

  if(DIRT2_SWITCH)
    include(platform/switch/switch.cmake)
  else()
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
  target_link_libraries(dirt2 PRIVATE "-framework CloudKit" "-framework Security" "-weak_framework MetalFX")
  target_include_directories(dirt2 PRIVATE "${DIRT2_MOLTENVK_LIB_DIR}/../include")
  target_link_libraries(dirt2 PRIVATE dirt2_shims dirt2_gen recomp_runtime dxvk_d3d9 SDL3::SDL3)
  if(DIRT2_HAVE_DXMT)
    # Object libraries' objects do not pass through other object libraries.
    target_link_libraries(dirt2 PRIVATE dirt2_shims_d3d11)
    if(NOT DIRT2_IOS)
      # DiRT2.app/Contents/Frameworks/libdxmt-native.dylib (package_macos.sh).
      target_link_options(dirt2 PRIVATE "-Wl,-rpath,@executable_path/../Frameworks")
    endif()
    # DX11 (DXMT) is the default renderer on Apple platforms (DIRT2_RENDERER=dx9 for DXVK).
    target_compile_definitions(dirt2 PRIVATE DIRT2_DEFAULT_DX11=1)
    if(DIRT2_IOS)
      # dxmt.framework ships in the app.
      target_link_options(dirt2 PRIVATE "-Wl,-rpath,@executable_path/Frameworks")
      set_target_properties(dirt2 PROPERTIES SKIP_BUILD_RPATH ON)  # no build-tree paths in the app
      add_custom_command(TARGET dirt2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_DIR:dirt2>/Frameworks
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${DIRT2_DXMT_DIR}/dxmt.framework"
          $<TARGET_BUNDLE_DIR:dirt2>/Frameworks/dxmt.framework
        VERBATIM)
    endif()
  endif()
  # Community gamepad mappings, loaded from next to the executable at startup.
  add_custom_command(TARGET dirt2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL_GameControllerDB/gamecontrollerdb.txt $<TARGET_FILE_DIR:dirt2>)
  # Shipped pipeline warm-up data (game-derived, never committed): DXVK's shader
  # cache and the merged pipeline list from tools/scripts/merge_pipelines.py.
  set(DIRT2_PIPELINE_DATA_DIR "${CMAKE_SOURCE_DIR}/build/pipelines" CACHE PATH
    "Folder with dirt2.dxvk.bin, dirt2.dxvk.lut and dirt2.pipelines to ship")
  # DX11 path (DXMT): dirt2.dxmt-pipelines and dxmt/shaders_*.db from
  # tools/scripts/merge_dxmt_pipelines.py.
  foreach(f dirt2.dxvk.bin dirt2.dxvk.lut dirt2.pipelines dirt2.dxmt-pipelines)
    if(EXISTS "${DIRT2_PIPELINE_DATA_DIR}/${f}")
      add_custom_command(TARGET dirt2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DIRT2_PIPELINE_DATA_DIR}/${f}" $<TARGET_FILE_DIR:dirt2>)
    endif()
  endforeach()
  if(IS_DIRECTORY "${DIRT2_PIPELINE_DATA_DIR}/dxmt")
    add_custom_command(TARGET dirt2 POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${DIRT2_PIPELINE_DATA_DIR}/dxmt" $<TARGET_FILE_DIR:dirt2>/dxmt)
  endif()
  endif()  # Apple
else()
  message(STATUS "generated/sources.txt not found: run 'recomp lift' to enable the dirt2 target")
endif()
