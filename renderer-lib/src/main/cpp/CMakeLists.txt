cmake_minimum_required(VERSION 3.22.1)
project("virtualcamerarenderer")

add_library(virtualcamerarenderer SHARED
    VirtualCameraRenderer.cpp
    VirtualCameraRendererJni.cpp
)

target_include_directories(virtualcamerarenderer PRIVATE
    ${CMAKE_SOURCE_DIR}
)

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(virtualcamerarenderer
    ${log-lib}
    ${android-lib}
)
