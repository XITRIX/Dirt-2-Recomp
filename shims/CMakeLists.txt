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
  xlive/xlive.cpp
  ws2_32/ws2_32.cpp
  d3d9/bridge.cpp)
target_link_libraries(dirt2_shims PUBLIC recomp_runtime dxvk_d3d9 SDL3::SDL3)
target_compile_options(dirt2_shims PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function)
