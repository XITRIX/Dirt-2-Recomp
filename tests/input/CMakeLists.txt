cmake_minimum_required(VERSION 3.25)
project(Dirt2TouchTests LANGUAGES CXX)
enable_testing()
# Only SDL's public headers are needed: the test supplies a deterministic device
# and window boundary. It can run on desktop hosts without a display or GPU.
find_path(SDL3_INCLUDE_DIR SDL3/SDL.h
  HINTS "${CMAKE_CURRENT_LIST_DIR}/../../build/deps/macos/include" REQUIRED)
add_executable(test_virtual_controller test_virtual_controller.cpp
  ../../platform/sdl/virtual_controller.cpp)
target_compile_features(test_virtual_controller PRIVATE cxx_std_20)
target_compile_definitions(test_virtual_controller PRIVATE DIRT2_TOUCH_CONTROLLER=1 SDL_STATIC_LIB=1)
target_include_directories(test_virtual_controller PRIVATE ../.. "${SDL3_INCLUDE_DIR}")
# Assertions are the test oracle, even with a Release configuration.
if(MSVC)
  target_compile_options(test_virtual_controller PRIVATE /UNDEBUG)
else()
  target_compile_options(test_virtual_controller PRIVATE -UNDEBUG -Wall -Wextra)
endif()
add_test(NAME virtual_controller COMMAND test_virtual_controller)
