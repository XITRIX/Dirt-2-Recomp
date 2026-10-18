# Import shims. An OBJECT library so every shim is linked and overrides the
# generated weak stubs (a static archive would only pull referenced members).
add_library(dirt2_shims OBJECT
  core.cpp
  vfs.cpp
  kernel32/sync.cpp
  kernel32/memory.cpp
  kernel32/file.cpp
  kernel32/process.cpp
  kernel32/locale.cpp
  user32/user32.cpp
  misc/misc.cpp
  misc/envmap_list.cpp
  xlive/xlive.cpp
  ws2_32/ws2_32.cpp
  d3d9/bridge.cpp
  frontend/menu_patch.cpp
  d3d11/reflect.cpp
  d3d11/compiler.cpp
  audio/dsound.cpp
  input/dinput8.cpp
  input/xinput.cpp
  d3dx9/d3dx9.cpp
  video/bink.cpp
  video/bink_decoder.cpp
  audio/openal.cpp
  audio/vorbisfile.cpp)
# Static libraries from build/deps/<platform> (build_deps.sh): link with their
# private dependencies.
find_package(PkgConfig REQUIRED)
pkg_check_modules(VORBISFILE REQUIRED vorbisfile)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil)
# pkg-config's "-framework X" pairs arrive as two list items: rejoin them.
set(_deps_ldflags)
set(_pending_framework OFF)
foreach(flag ${VORBISFILE_STATIC_LDFLAGS} ${FFMPEG_STATIC_LDFLAGS})
  if(_pending_framework)
    list(APPEND _deps_ldflags "-framework ${flag}")
    set(_pending_framework OFF)
  elseif(flag STREQUAL "-framework")
    set(_pending_framework ON)
  else()
    list(APPEND _deps_ldflags "${flag}")
  endif()
endforeach()
target_include_directories(dirt2_shims PUBLIC ${VORBISFILE_STATIC_INCLUDE_DIRS} ${FFMPEG_STATIC_INCLUDE_DIRS})
target_link_libraries(dirt2_shims PUBLIC recomp_runtime dxvk_d3d9 SDL3::SDL3 OpenAL::OpenAL ${_deps_ldflags})
target_compile_options(dirt2_shims PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function)

# The D3D11/DXGI bridge compiles against DXMT's headers only: DXVK's include
# path has its own d3d11.h, and both declare IUnknown.
if(DIRT2_HAVE_DXMT)
  add_library(dirt2_shims_d3d11 OBJECT d3d11/bridge.cpp)
  target_compile_definitions(dirt2_shims_d3d11 PRIVATE NOMINMAX _WIN32_WINNT=0xa00)
  target_include_directories(dirt2_shims_d3d11 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(dirt2_shims_d3d11 PUBLIC recomp_runtime dxmt)
  target_compile_options(dirt2_shims_d3d11 PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function)
endif()
