#pragma once

#include <cstdint>
#include <atomic>

namespace vcam {

// Shared memory header - must match HAL's FrameHeader
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
static constexpr uint32_t FORMAT_RGBA_8888 = 1;

class VirtualCameraWriter {
public:
    VirtualCameraWriter();
    ~VirtualCameraWriter();
    
    bool initialize(uint32_t width, uint32_t height);
    void shutdown();
    void writeFrame(const uint8_t* rgbaData, size_t size);
    
    uint32_t getWidth() const { return mWidth; }
    uint32_t getHeight() const { return mHeight; }
    bool isInitialized() const { return mMappedAddr != nullptr; }
    
private:
    bool sendFdToHal();
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
