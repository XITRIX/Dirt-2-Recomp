cmake_minimum_required(VERSION 3.25)
project(Dirt2Recomp LANGUAGES C CXX)

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

  add_executable(dirt2 runtime/src/main.cpp)
  target_link_libraries(dirt2 PRIVATE dirt2_shims dirt2_gen recomp_runtime)
else()
  message(STATUS "generated/sources.txt not found: run 'recomp lift' to enable the dirt2 target")
endif()
