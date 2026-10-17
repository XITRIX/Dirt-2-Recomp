add_library(recomp_runtime STATIC
  src/memory.cpp
  src/heap.cpp
  src/loader.cpp
  src/dispatch.cpp
  src/thread.cpp
  src/cpu.cpp)
target_include_directories(recomp_runtime PUBLIC include)
target_compile_options(recomp_runtime PRIVATE -Wall -Wno-unused-parameter)
find_package(Threads REQUIRED)
target_link_libraries(recomp_runtime PUBLIC Threads::Threads)
