FetchContent_Declare(zydis
  GIT_REPOSITORY https://github.com/zyantific/zydis.git
  GIT_TAG v4.1.1
  GIT_SHALLOW TRUE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zydis)

add_executable(recomp
  main.cpp
  pe/pe_image.cpp
  analysis/disasm.cpp
  analysis/discovery.cpp
  analysis/report.cpp
  codegen/fn_lifter.cpp
  codegen/lift_base.cpp
  codegen/lift_sse.cpp
  codegen/lift_x87.cpp
  codegen/lifter.cpp)
target_include_directories(recomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/runtime/include)
target_link_libraries(recomp PRIVATE Zydis)
target_compile_options(recomp PRIVATE -Wall -Wextra -Wno-unused-parameter)
