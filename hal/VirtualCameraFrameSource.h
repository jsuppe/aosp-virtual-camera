/*
 * VirtualCameraFrameSource - Shared memory interface for renderer frames
 * 
 * Renderer writes frames to /dev/ashmem or ion buffer
 * HAL reads frames from the same buffer
 * Simple flag-based synchronization
 */

#pragma once

#include <cstdint>
#include <string>
#include <mutex>
#include <atomic>

namespace aidl::android::hardware::camera::provider::implementation {

// Shared memory header structure
struct FrameHeader {
    std::atomic<uint32_t> magic;          // 0x56434D46 = "VCMF"
    std::atomic<uint32_t> version;        // Protocol version
    std::atomic<uint32_t> width;
    std::atomic<uint32_t> height;
    std::atomic<uint32_t> format;         // HAL pixel format
    std::atomic<uint32_t> stride;
    std::atomic<uint64_t> frameNumber;
    std::atomic<uint64_t> timestamp;
    std::atomic<uint32_t> dataOffset;     // Offset to frame data
    std::atomic<uint32_t> dataSize;       // Size of frame data
    std::atomic<uint32_t> flags;          // NEW_FRAME = 1, RENDERER_ACTIVE = 2
};

static constexpr uint32_t VCMF_MAGIC = 0x56434D46;
static constexpr uint32_t VCMF_VERSION = 1;
static constexpr uint32_t FLAG_NEW_FRAME = 1;
static constexpr uint32_t FLAG_RENDERER_ACTIVE = 2;

class VirtualCameraFrameSource {
public:
    VirtualCameraFrameSource();
    ~VirtualCameraFrameSource();
    
    // Check if renderer is connected and has frames
    bool isRendererActive();
    
    // Try to acquire latest frame from renderer
    // Returns true if new frame available, copies to destBuffer
    bool acquireFrame(void* destBuffer, size_t destSize,
                      uint32_t* outWidth, uint32_t* outHeight,
                      uint64_t* outTimestamp);
    
    // Get frame dimensions without copying
    bool getFrameInfo(uint32_t* width, uint32_t* height, uint32_t* format);
    
private:
    bool mapSharedMemory();
    void unmapSharedMemory();
    
    int mFd = -1;
    void* mMappedAddr = nullptr;
    size_t mMappedSize = 0;
    FrameHeader* mHeader = nullptr;
    void* mFrameData = nullptr;
    
    std::mutex mLock;
    
    // Path for shared memory - renderer creates, HAL opens
    static constexpr const char* SHARED_MEM_PATH = "/dev/virtual_camera_shm";
};

}  // namespace
