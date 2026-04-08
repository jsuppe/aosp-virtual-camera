/*
 * VirtualCameraFrameSource - Shared memory interface for renderer frames
 * 
 * Receives ashmem fd from renderer via Unix socket.
 * Maps and reads frames from the shared memory.
 */

#pragma once

#include <cstdint>
#include <string>
#include <mutex>
#include <atomic>
#include <memory>

namespace aidl::android::hardware::camera::provider::implementation {

// Forward declaration
class VirtualCameraSocket;

// Shared memory header structure (must match renderer)
struct FrameHeader {
    std::atomic<uint32_t> magic;          // 0x56434D46 = "VCMF"
    std::atomic<uint32_t> version;        // Protocol version
    std::atomic<uint32_t> width;
    std::atomic<uint32_t> height;
    std::atomic<uint32_t> format;         // Renderer's output pixel format
    std::atomic<uint32_t> stride;
    std::atomic<uint64_t> frameNumber;
    std::atomic<uint64_t> timestamp;
    std::atomic<uint32_t> dataOffset;     // Offset to frame data
    std::atomic<uint32_t> dataSize;       // Size of frame data
    std::atomic<uint32_t> flags;          // RENDERER_ACTIVE = 2
    // Format negotiation (HAL → Renderer): HAL writes the consumer's
    // desired format here; renderer polls and switches output format.
    std::atomic<uint32_t> requestedFormat;  // FORMAT_* constant, 0 = no preference
    std::atomic<uint32_t> requestedWidth;
    std::atomic<uint32_t> requestedHeight;
};

static constexpr uint32_t VCMF_MAGIC = 0x56434D46;
static constexpr uint32_t VCMF_VERSION = 1;
static constexpr uint32_t FLAG_NEW_FRAME = 1;
static constexpr uint32_t FLAG_RENDERER_ACTIVE = 2;
static constexpr uint32_t FORMAT_RGBA_8888 = 1;
static constexpr uint32_t FORMAT_YUV_420 = 2;  // NV12 layout: Y plane, then interleaved UV

class VirtualCameraFrameSource {
public:
    VirtualCameraFrameSource();
    ~VirtualCameraFrameSource();
    
    // Start the socket server to receive renderer connections
    bool start();
    
    // Stop and clean up
    void stop();
    
    // Check if renderer is connected and has frames
    bool isRendererActive();
    
    // Try to acquire latest frame from renderer
    // Returns true if frame available, copies to destBuffer
    bool acquireFrame(void* destBuffer, size_t destSize,
                      uint32_t* outWidth, uint32_t* outHeight,
                      uint64_t* outTimestamp);
    
    // Get frame dimensions without copying
    bool getFrameInfo(uint32_t* width, uint32_t* height, uint32_t* format);

    // Request a specific format from the renderer (HAL → Renderer negotiation)
    void requestFormat(uint32_t format, uint32_t width, uint32_t height);
    
private:
    void onFdReceived(int fd, size_t size);
    bool mapSharedMemory(int fd, size_t size);
    void unmapSharedMemory();
    
    std::unique_ptr<VirtualCameraSocket> mSocket;
    
    int mShmFd = -1;
    void* mMappedAddr = nullptr;
    size_t mMappedSize = 0;
    FrameHeader* mHeader = nullptr;
    void* mFrameData = nullptr;
    uint64_t mLastReadTimestamp = 0;
    
    std::mutex mLock;
};

}  // namespace
