/*
 * VirtualCameraRenderer - Client library for apps to provide frames to virtual camera
 * 
 * Usage:
 *   1. Create VirtualCameraRenderer instance
 *   2. Call initialize() with desired resolution
 *   3. For each frame, call beginFrame() to get buffer pointer
 *   4. Write frame data (YUV420 or RGBA)
 *   5. Call endFrame() to signal frame is ready
 */

#pragma once

#include <cstdint>
#include <string>
#include <mutex>

namespace virtualcamera {

// Must match HAL-side definitions
struct FrameHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
    uint64_t frameNumber;
    uint64_t timestamp;
    uint32_t dataOffset;
    uint32_t dataSize;
    uint32_t flags;
};

static constexpr uint32_t VCMF_MAGIC = 0x56434D46;
static constexpr uint32_t VCMF_VERSION = 1;
static constexpr uint32_t FLAG_NEW_FRAME = 1;
static constexpr uint32_t FLAG_RENDERER_ACTIVE = 2;

// Pixel formats (matching HAL)
enum class PixelFormat : uint32_t {
    RGBA_8888 = 1,
    YUV_420_888 = 35,
    IMPLEMENTATION_DEFINED = 34,
};

class VirtualCameraRenderer {
public:
    VirtualCameraRenderer();
    ~VirtualCameraRenderer();
    
    // Initialize shared memory with given dimensions
    // Returns true on success
    bool initialize(uint32_t width, uint32_t height, PixelFormat format);
    
    // Shutdown and release resources
    void shutdown();
    
    // Check if initialized and ready
    bool isReady() const { return mInitialized; }
    
    // Get frame buffer for writing
    // Returns nullptr if not initialized
    void* beginFrame();
    
    // Signal that frame is complete
    // timestamp: frame timestamp in nanoseconds (0 = auto)
    void endFrame(uint64_t timestamp = 0);
    
    // Get frame dimensions
    uint32_t getWidth() const { return mWidth; }
    uint32_t getHeight() const { return mHeight; }
    uint32_t getStride() const { return mStride; }
    size_t getFrameSize() const { return mFrameSize; }
    
private:
    bool createSharedMemory();
    void destroySharedMemory();
    size_t calculateFrameSize(uint32_t width, uint32_t height, PixelFormat format);
    
    bool mInitialized = false;
    int mFd = -1;
    void* mMappedAddr = nullptr;
    size_t mMappedSize = 0;
    
    FrameHeader* mHeader = nullptr;
    void* mFrameData = nullptr;
    
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    uint32_t mStride = 0;
    size_t mFrameSize = 0;
    PixelFormat mFormat = PixelFormat::YUV_420_888;
    uint64_t mFrameNumber = 0;
    
    std::mutex mLock;
    
    static constexpr const char* SHARED_MEM_PATH = "/dev/virtual_camera_shm";
};

}  // namespace virtualcamera
