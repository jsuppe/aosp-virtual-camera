cmake_minimum_required(VERSION 3.22.1)
project("vcam_unified_test")

# Find required packages
find_library(log-lib log)
find_library(android-lib android)

# Vulkan from NDK
find_package(Vulkan REQUIRED)

add_library(${CMAKE_PROJECT_NAME} SHARED
    unified_renderer.cpp
    vulkan_cube.cpp
    virtual_camera_writer.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(${CMAKE_PROJECT_NAME}
    ${log-lib}
    ${android-lib}
    Vulkan::Vulkan
    nativewindow   # For AHardwareBuffer APIs
)
