/*
 * VirtualCameraFrameSource - Shared memory interface for renderer frames
 */

#define LOG_TAG "VirtualCameraFrameSource"

#include "VirtualCameraFrameSource.h"

#include <log/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace aidl::android::hardware::camera::provider::implementation {

// Maximum shared memory size: 4K RGBA = 4096 * 2160 * 4 + header
static constexpr size_t MAX_SHM_SIZE = 4096 * 2160 * 4 + 4096;

VirtualCameraFrameSource::VirtualCameraFrameSource() {
    ALOGI("VirtualCameraFrameSource created");
}

VirtualCameraFrameSource::~VirtualCameraFrameSource() {
    unmapSharedMemory();
    ALOGI("VirtualCameraFrameSource destroyed");
}

bool VirtualCameraFrameSource::mapSharedMemory() {
    std::lock_guard<std::mutex> lock(mLock);
    
    if (mMappedAddr != nullptr) {
        return true;  // Already mapped
    }
    
    // Try to open existing shared memory created by renderer
    mFd = open(SHARED_MEM_PATH, O_RDONLY);
    if (mFd < 0) {
        // Not an error - renderer just isn't running yet
        return false;
    }
    
    // Get file size
    struct stat st;
    if (fstat(mFd, &st) < 0) {
        ALOGE("Failed to stat shared memory: %s", strerror(errno));
        close(mFd);
        mFd = -1;
        return false;
    }
    
    mMappedSize = st.st_size;
    if (mMappedSize < sizeof(FrameHeader)) {
        ALOGE("Shared memory too small: %zu bytes", mMappedSize);
        close(mFd);
        mFd = -1;
        return false;
    }
    
    // Map the shared memory read-only
    mMappedAddr = mmap(nullptr, mMappedSize, PROT_READ, MAP_SHARED, mFd, 0);
    if (mMappedAddr == MAP_FAILED) {
        ALOGE("Failed to mmap shared memory: %s", strerror(errno));
        mMappedAddr = nullptr;
        close(mFd);
        mFd = -1;
        return false;
    }
    
    mHeader = static_cast<FrameHeader*>(mMappedAddr);
    
    // Verify magic number
    if (mHeader->magic.load(std::memory_order_acquire) != VCMF_MAGIC) {
        ALOGE("Invalid magic number in shared memory");
        unmapSharedMemory();
        return false;
    }
    
    // Verify version
    if (mHeader->version.load(std::memory_order_acquire) != VCMF_VERSION) {
        ALOGE("Unsupported shared memory version: %u", 
              mHeader->version.load(std::memory_order_acquire));
        unmapSharedMemory();
        return false;
    }
    
    // Calculate frame data pointer
    uint32_t dataOffset = mHeader->dataOffset.load(std::memory_order_acquire);
    if (dataOffset >= mMappedSize) {
        ALOGE("Invalid data offset: %u", dataOffset);
        unmapSharedMemory();
        return false;
    }
    
    mFrameData = static_cast<uint8_t*>(mMappedAddr) + dataOffset;
    
    ALOGI("Mapped shared memory: %zu bytes, data offset: %u", mMappedSize, dataOffset);
    return true;
}

void VirtualCameraFrameSource::unmapSharedMemory() {
    std::lock_guard<std::mutex> lock(mLock);
    
    if (mMappedAddr != nullptr) {
        munmap(mMappedAddr, mMappedSize);
        mMappedAddr = nullptr;
        mHeader = nullptr;
        mFrameData = nullptr;
        mMappedSize = 0;
    }
    
    if (mFd >= 0) {
        close(mFd);
        mFd = -1;
    }
}

bool VirtualCameraFrameSource::isRendererActive() {
    // Try to map if not already mapped
    if (mMappedAddr == nullptr) {
        if (!mapSharedMemory()) {
            return false;
        }
    }
    
    if (mHeader == nullptr) {
        return false;
    }
    
    // Check if renderer has marked itself as active
    uint32_t flags = mHeader->flags.load(std::memory_order_acquire);
    return (flags & FLAG_RENDERER_ACTIVE) != 0;
}

bool VirtualCameraFrameSource::acquireFrame(
        void* destBuffer, size_t destSize,
        uint32_t* outWidth, uint32_t* outHeight,
        uint64_t* outTimestamp) {
    
    if (!isRendererActive()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mLock);
    
    if (mHeader == nullptr || mFrameData == nullptr) {
        return false;
    }
    
    // Read frame info
    uint32_t width = mHeader->width.load(std::memory_order_acquire);
    uint32_t height = mHeader->height.load(std::memory_order_acquire);
    uint32_t dataSize = mHeader->dataSize.load(std::memory_order_acquire);
    uint64_t timestamp = mHeader->timestamp.load(std::memory_order_acquire);
    
    // Track timestamp for logging, but always provide the current frame
    // (Camera reads faster than renderer writes, so we reuse frames)
    bool isNewFrame = (timestamp != mLastReadTimestamp);
    mLastReadTimestamp = timestamp;
    
    if (outWidth) *outWidth = width;
    if (outHeight) *outHeight = height;
    if (outTimestamp) *outTimestamp = timestamp;
    
    // Validate data size
    if (dataSize > destSize) {
        ALOGE("Frame data too large: %u > %zu", dataSize, destSize);
        return false;
    }
    
    // Copy frame data
    memcpy(destBuffer, mFrameData, dataSize);
    
    ALOGV("Acquired frame: %ux%u, %u bytes, ts=%lu", width, height, dataSize, 
          (unsigned long)timestamp);
    return true;
}

bool VirtualCameraFrameSource::getFrameInfo(
        uint32_t* width, uint32_t* height, uint32_t* format) {
    
    if (!isRendererActive()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mLock);
    
    if (mHeader == nullptr) {
        return false;
    }
    
    if (width) *width = mHeader->width.load(std::memory_order_acquire);
    if (height) *height = mHeader->height.load(std::memory_order_acquire);
    if (format) *format = mHeader->format.load(std::memory_order_acquire);
    
    return true;
}

}  // namespace
