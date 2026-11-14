# Audio, DX11 clear/readback/presentation and teardown, without game data.
add_executable(dirt2_dx11_probe EXCLUDE_FROM_ALL
    platform/switch/dxvk_test_main.cpp platform/switch/dxvk_checks.cpp
    platform/switch/dxvk_preflight.cpp)
target_include_directories(dirt2_dx11_probe PRIVATE "${CMAKE_SOURCE_DIR}/runtime/include")
target_link_libraries(dirt2_dx11_probe PRIVATE dxvk_d3d9 SDL3::SDL3 OpenAL::OpenAL)
nx_generate_nacp(dirt2_dx11_probe.nacp NAME "DiRT 2 Switch 2 GPU test"
    AUTHOR "Dirt2Recomp" VERSION "0.1.0")
nx_create_nro(dirt2_dx11_probe NACP dirt2_dx11_probe.nacp)
