/*
 * VirtualCameraClient - Renderer-side client for virtual camera
 * 
 * Creates shared memory and sends fd to HAL via Unix socket.
 * Then writes frames to shared memory for HAL to read.
 */

#pragma once

#include <cstdint>
#include <atomic>

namespace vcam {

// Frame header in shared memory (must match HAL)
struct FrameHeader {
    std::atomic<uint32_t> magic;          // 0x56434D46 = "VCMF"
    std::atomic<uint32_t> version;        // Protocol version
    std::atomic<uint32_t> width;
    std::atomic<uint32_t> height;
    std::atomic<uint32_t> format;         // Pixel format
    std::atomic<uint32_t> stride;
    std::atomic<uint64_t> frameNumber;
    std::atomic<uint64_t> timestamp;
    std::atomic<uint32_t> dataOffset;     // Offset to frame data
    std::atomic<uint32_t> dataSize;       // Size of frame data
    std::atomic<uint32_t> flags;          // RENDERER_ACTIVE = 2
};

static constexpr uint32_t VCMF_MAGIC = 0x56434D46;  // "VCMF"
static constexpr uint32_t VCMF_VERSION = 1;
static constexpr uint32_t FORMAT_RGBA_8888 = 1;
static constexpr uint32_t FLAG_RENDERER_ACTIVE = 2;

class VirtualCameraClient {
public:
    VirtualCameraClient();
    ~VirtualCameraClient();
    
    // Initialize with frame dimensions
    // Creates ashmem and connects to HAL
    bool initialize(uint32_t width, uint32_t height);
    
    // Shutdown and disconnect
    void shutdown();
    
    // Write a frame (RGBA format)
    void writeFrame(const uint8_t* rgbaData, size_t size);
    
    // Get dimensions
    uint32_t getWidth() const { return mWidth; }
    uint32_t getHeight() const { return mHeight; }
    
    // Check if connected to HAL
    bool isConnected() const { return mConnected; }

private:
    bool createSharedMemory();
    bool connectToHal();
    
    int mShmFd = -1;
    void* mMappedAddr = nullptr;
    size_t mMappedSize = 0;
    FrameHeader* mHeader = nullptr;
    uint8_t* mFrameData = nullptr;
    
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    uint64_t mFrameNumber = 0;
    bool mConnected = false;
    
    static constexpr const char* SOCKET_PATH = "/data/local/tmp/virtual_camera.sock";
};

}  // namespace vcam
