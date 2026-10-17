# Differential tests: lifted C for each case in cases.txt against Unicorn.
#
#   cases.txt        case name, Intel-syntax asm, harness options
#   cases.bin.txt    machine code for each case (regenerate with assemble.py)
#   lifter_testgen   lifts every case with the real FnLifter into lifter_cases.c
#   lifter_difftest  runs each case in Unicorn and in the lifted C from random states

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(UNICORN QUIET IMPORTED_TARGET unicorn>=2.0)
endif()
if(UNICORN_FOUND)
  set(DIRT2_UNICORN PkgConfig::UNICORN)
else()
  FetchContent_Declare(unicorn
    GIT_REPOSITORY https://github.com/unicorn-engine/unicorn.git
    GIT_TAG 2.1.1
    GIT_SHALLOW TRUE)
  set(UNICORN_ARCH "x86" CACHE STRING "" FORCE)
  set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(unicorn)
  set(DIRT2_UNICORN unicorn)
endif()

add_executable(lifter_testgen testgen.cpp)
target_link_libraries(lifter_testgen PRIVATE recomp_core)
target_compile_options(lifter_testgen PRIVATE -Wall -Wextra -Wno-unused-parameter)

set(_gen ${CMAKE_CURRENT_BINARY_DIR}/lifter_cases.c)
add_custom_command(OUTPUT ${_gen}
  COMMAND lifter_testgen ${CMAKE_CURRENT_SOURCE_DIR}/cases.txt ${CMAKE_CURRENT_SOURCE_DIR}/cases.bin.txt ${_gen}
  DEPENDS lifter_testgen ${CMAKE_CURRENT_SOURCE_DIR}/cases.txt ${CMAKE_CURRENT_SOURCE_DIR}/cases.bin.txt
  COMMENT "Lifting lifter test cases")
# Same flags as the game build (dirt2_gen).
set_source_files_properties(${_gen} PROPERTIES COMPILE_OPTIONS "-w;-O2;-fno-strict-aliasing")

add_executable(lifter_difftest runner.cpp ${_gen})
target_include_directories(lifter_difftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/runtime/include)
target_link_libraries(lifter_difftest PRIVATE ${DIRT2_UNICORN})
target_compile_options(lifter_difftest PRIVATE -Wall -Wno-unused-parameter)

# Sharded so `ctest -j` spreads the cases over cores.
set(_shards 8)
math(EXPR _last "${_shards} - 1")
foreach(i RANGE ${_last})
  add_test(NAME lifter_difftest_${i} COMMAND lifter_difftest --shard ${i}/${_shards})
endforeach()
