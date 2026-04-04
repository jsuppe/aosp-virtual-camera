cmake_minimum_required(VERSION 3.22.1)
project("vcam_unified_test")

# VM Producer library (for VirtualMediaService integration)
add_library(vmproducer SHARED
    virtual_media_producer.cpp
    audio_producer.cpp
    video_producer.cpp
)

target_link_libraries(vmproducer
    android
    log
    nativewindow
)
