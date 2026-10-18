# Host tool: Direct3D 9 shader bytecode -> GLSL 4.60 for deko3d (dx9shader.cpp).
# Built by tools/scripts/build_shader_tools.sh from pinned sources:
#   MOJOSHADER_DIR    MojoShader (SPIR-V profile only)
#   SPIRV_CROSS_DIR   SPIRV-Cross (MoltenVK's pin, build/src/MoltenVK/External/SPIRV-Cross)
cmake_minimum_required(VERSION 3.20)
project(dx9shader C CXX)
set(CMAKE_CXX_STANDARD 17)

set(MOJOSHADER_DIR "" CACHE PATH "MojoShader source")
set(SPIRV_CROSS_DIR "" CACHE PATH "SPIRV-Cross source")

add_library(mojoshader STATIC
  ${MOJOSHADER_DIR}/mojoshader.c
  ${MOJOSHADER_DIR}/mojoshader_common.c
  ${MOJOSHADER_DIR}/profiles/mojoshader_profile_spirv.c
  ${MOJOSHADER_DIR}/profiles/mojoshader_profile_common.c)
target_include_directories(mojoshader PUBLIC ${MOJOSHADER_DIR} ${MOJOSHADER_DIR}/profiles)
foreach(p D3D BYTECODE HLSL GLSL120 GLSLES GLSLES3 GLSL ARB1 ARB1_NV METAL GLSPIRV)
  target_compile_definitions(mojoshader PUBLIC SUPPORT_PROFILE_${p}=0)
endforeach()
target_compile_definitions(mojoshader PUBLIC SUPPORT_PROFILE_SPIRV=1 MOJOSHADER_NO_VERSION_INCLUDE)
target_compile_options(mojoshader PRIVATE -w)

add_library(spirv_cross_glsl STATIC
  ${SPIRV_CROSS_DIR}/spirv_cross.cpp
  ${SPIRV_CROSS_DIR}/spirv_parser.cpp
  ${SPIRV_CROSS_DIR}/spirv_cross_parsed_ir.cpp
  ${SPIRV_CROSS_DIR}/spirv_cfg.cpp
  ${SPIRV_CROSS_DIR}/spirv_glsl.cpp)
target_include_directories(spirv_cross_glsl PUBLIC ${SPIRV_CROSS_DIR})

add_executable(dx9shader dx9shader.cpp)
target_link_libraries(dx9shader PRIVATE mojoshader spirv_cross_glsl)
