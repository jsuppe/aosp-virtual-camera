cmake_minimum_required(VERSION 3.22.1)

project("vcamrenderer")

# Find Vulkan
find_package(Vulkan REQUIRED)

# Shader compilation (SPIR-V)
set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SPIRV_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SPIRV_DIR})

add_library(vcamrenderer SHARED
    vcam_renderer.cpp
    vulkan_renderer.cpp
    cube_geometry.cpp
)

target_include_directories(vcamrenderer PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(vcamrenderer
    android
    log
    Vulkan::Vulkan
    nativewindow
)

target_compile_options(vcamrenderer PRIVATE
    -Wall
    -Werror
    -DVK_USE_PLATFORM_ANDROID_KHR
)
