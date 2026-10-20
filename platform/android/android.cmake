# SDLActivity loads SDL3, libc++_shared, then this shared game image.
add_library(dirt2 SHARED platform/sdl/sdl_main.cpp platform/android/services.cpp)
target_compile_definitions(dirt2 PRIVATE DIRT2_DEFAULT_DX11=1)
target_link_libraries(dirt2 PRIVATE dirt2_shims dirt2_shims_d3d11 dirt2_gen recomp_runtime dxvk_d3d9 SDL3::SDL3 android log)
target_link_options(dirt2 PRIVATE "LINKER:--build-id=sha1" "LINKER:--exclude-libs=ALL" "LINKER:-z,max-page-size=16384")

# Static runtime/codec/DXVK symbols are private to this image. In particular,
# FFmpeg NEON assembly references internal tables with direct PIC relocations;
# these must bind locally rather than be interposable shared-library exports.
