/*
 * VirtualCameraWriter - Write frames to shared memory for HAL consumption
 * 
 * Usage:
 *   VirtualCameraWriter writer;
 *   if (writer.initialize(1920, 1080)) {
 *       while (rendering) {
 *           writer.beginFrame();
 *           // ... render to writer.getBuffer() ...
 *           writer.endFrame();
 *       }
 *   }
 */

#pragma once

#include <cstdint>
#include <atomic>

namespace vcam {

// Must match HAL's FrameHeader
struct FrameHeader {
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;
    std::atomic<uint32_t> width;
    std::atomic<uint32_t> height;
    std::atomic<uint32_t> format;
    std::atomic<uint32_t> stride;
    std::atomic<uint64_t> frameNumber;
    std::atomic<uint64_t> timestamp;
    std::atomic<uint32_t> dataOffset;
    std::atomic<uint32_t> dataSize;
    std::atomic<uint32_t> flags;
};

static constexpr uint32_t VCMF_MAGIC = 0x56434D46;  // "VCMF"
static constexpr uint32_t VCMF_VERSION = 1;
static constexpr uint32_t FLAG_NEW_FRAME = 1;
static constexpr uint32_t FLAG_RENDERER_ACTIVE = 2;

// Pixel format constants (match HAL)
static constexpr uint32_t FORMAT_RGBA_8888 = 1;

class VirtualCameraWriter {
public:
    VirtualCameraWriter();
    ~VirtualCameraWriter();
    
    // Initialize with frame dimensions. Returns true on success.
    bool initialize(uint32_t width, uint32_t height);
    
    // Shutdown and release shared memory
    void shutdown();
    
    // Begin writing a frame. Returns pointer to RGBA buffer.
    uint8_t* beginFrame();
    
    // End frame and signal HAL that new frame is available
    void endFrame();
    
    // Get frame buffer (valid between beginFrame/endFrame)
    uint8_t* getBuffer() { return mFrameData; }
    
    // Get dimensions
    uint32_t getWidth() const { return mWidth; }
    uint32_t getHeight() const { return mHeight; }
    size_t getBufferSize() const { return mWidth * mHeight * 4; }
    
    // Check if initialized
    bool isInitialized() const { return mMappedAddr != nullptr; }
    
private:
    static constexpr const char* SHARED_MEM_PATH = "/dev/virtual_camera_shm";
    
    int mFd = -1;
    void* mMappedAddr = nullptr;
    size_t mMappedSize = 0;
    FrameHeader* mHeader = nullptr;
    uint8_t* mFrameData = nullptr;
    
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    uint64_t mFrameNumber = 0;
};

}  // namespace vcam
