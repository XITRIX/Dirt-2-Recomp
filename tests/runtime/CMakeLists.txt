find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME runtime_critical_section_generation COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/test_critical_sections.py
  ${CMAKE_SOURCE_DIR}/shims/kernel32/sync.cpp ${CMAKE_CXX_COMPILER} "${CMAKE_OSX_SYSROOT}")
add_executable(runtime_dispatch_cache test_dispatch_cache.cpp ${CMAKE_SOURCE_DIR}/runtime/src/dispatch.cpp)
target_include_directories(runtime_dispatch_cache PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include)
target_link_libraries(runtime_dispatch_cache PRIVATE Threads::Threads)
add_test(NAME runtime_dispatch_cache COMMAND runtime_dispatch_cache)
add_library(memory_baseline OBJECT memory_variant.c)
add_library(memory_inline OBJECT memory_variant.c)
foreach(target memory_baseline memory_inline)
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include)
  target_compile_definitions(${target} PRIVATE RECOMP_TSO=1)
  target_compile_options(${target} PRIVATE -O2)
endforeach()
target_compile_definitions(memory_inline PRIVATE RECOMP_INLINE_GUEST_MEMORY=1 VARIANT=inline_memory)
add_executable(runtime_memory_inline test_memory_inline.cpp $<TARGET_OBJECTS:memory_baseline> $<TARGET_OBJECTS:memory_inline>)
add_test(NAME runtime_memory_inline COMMAND runtime_memory_inline)

add_executable(runtime_pc_sampling test_pc_sampling.cpp)
target_include_directories(runtime_pc_sampling PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include)
add_test(NAME runtime_pc_sampling COMMAND runtime_pc_sampling)

# Assertions are the regression checks, including release test builds.
foreach(target runtime_dispatch_cache runtime_memory_inline runtime_pc_sampling)
  target_compile_options(${target} PRIVATE -UNDEBUG)
endforeach()

# Compare the experimental scheduler against the game's existing parallel map.
if(EXISTS "${DIRT2_GAME_DIR}/system/workerMap8Core.xml")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(switch_worker_topology ../switch/test_worker_topology.cpp)
  target_include_directories(switch_worker_topology PRIVATE ${CMAKE_SOURCE_DIR})
  add_test(NAME switch_worker_topology COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/switch/test_worker_topology.py $<TARGET_FILE:switch_worker_topology>
    "${DIRT2_GAME_DIR}/system/workerMap8Core.xml")
endif()

add_executable(d3d11_shadow_write ../d3d11/test_shadow_write.cpp)
target_include_directories(d3d11_shadow_write PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(d3d11_shadow_write PRIVATE -UNDEBUG)
add_test(NAME d3d11_shadow_write COMMAND d3d11_shadow_write)
