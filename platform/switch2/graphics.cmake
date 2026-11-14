# Native Switch 2: libounce and OunceVK from the supplied ounce SDK.
# Do not use the isolated Switch 1 NVK SDK or its renamed Mesa archive.
set(DIRT2_DXVK_DIR "${CMAKE_SOURCE_DIR}/build/src/dxvk-switch2")
set(DIRT2_DXVK_SWITCH_ARCHIVE "${CMAKE_SOURCE_DIR}/build/dxvk/switch2/libdxvk_switch2.a"
    CACHE FILEPATH "Native Switch 2 DXVK archive")
find_library(DIRT2_OUNCE_VULKAN NAMES vulkan
    PATHS "${DEVKITPRO}/portlibs/switch2/lib" NO_DEFAULT_PATH REQUIRED)
add_library(switch2_vulkan STATIC IMPORTED)
set_target_properties(switch2_vulkan PROPERTIES
    IMPORTED_LOCATION "${DIRT2_OUNCE_VULKAN}"
    INTERFACE_INCLUDE_DIRECTORIES "${DEVKITPRO}/portlibs/switch2/include")
add_library(vulkan ALIAS switch2_vulkan)
add_library(dxvk_d3d9 STATIC IMPORTED)
set_target_properties(dxvk_d3d9 PROPERTIES
    IMPORTED_LOCATION "${DIRT2_DXVK_SWITCH_ARCHIVE}"
    INTERFACE_INCLUDE_DIRECTORIES "${DIRT2_DXVK_DIR}/include/native/directx;${DIRT2_DXVK_DIR}/include/native/windows"
    INTERFACE_LINK_LIBRARIES switch2_vulkan)
set(DIRT2_HAVE_DXVK_D3D11 ON)
