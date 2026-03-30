/*
 * VirtualCameraFrameSource - Ashmem-based shared memory via socket
 */

#define LOG_TAG "VirtualCameraFrameSource"

#include "VirtualCameraFrameSource.h"
#include "VirtualCameraSocket.h"

#include <log/log.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace aidl::android::hardware::camera::provider::implementation {

VirtualCameraFrameSource::VirtualCameraFrameSource()
    : mSocket(std::make_unique<VirtualCameraSocket>()) {
    ALOGI("VirtualCameraFrameSource created");
}

VirtualCameraFrameSource::~VirtualCameraFrameSource() {
    stop();
    ALOGI("VirtualCameraFrameSource destroyed");
}

bool VirtualCameraFrameSource::start() {
    // Set up callback for when renderer connects
    mSocket->setOnFdReceived([this](int fd, size_t size) {
        onFdReceived(fd, size);
    });
    
    if (!mSocket->start()) {
        ALOGE("Failed to start socket server");
        return false;
    }
    
    ALOGI("FrameSource started, waiting for renderer...");
    return true;
}

void VirtualCameraFrameSource::stop() {
    mSocket->stop();
    unmapSharedMemory();
}

void VirtualCameraFrameSource::onFdReceived(int fd, size_t size) {
    std::lock_guard<std::mutex> lock(mLock);
    
    // Unmap old memory if any
    unmapSharedMemory();
    
    // Map the new fd
    if (!mapSharedMemory(fd, size)) {
        ALOGE("Failed to map received fd");
        close(fd);
        return;
    }
    
    mShmFd = fd;
    ALOGI("Renderer connected, mapped %zu bytes", size);
}

bool VirtualCameraFrameSource::mapSharedMemory(int fd, size_t size) {
    mMappedAddr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (mMappedAddr == MAP_FAILED) {
        ALOGE("Failed to mmap: %s", strerror(errno));
        mMappedAddr = nullptr;
        return false;
    }
    
    mMappedSize = size;
    mHeader = static_cast<FrameHeader*>(mMappedAddr);
    
    // Verify magic number
    if (mHeader->magic.load(std::memory_order_acquire) != VCMF_MAGIC) {
        ALOGE("Invalid magic number in shared memory");
        munmap(mMappedAddr, mMappedSize);
        mMappedAddr = nullptr;
        mHeader = nullptr;
        return false;
    }
    
    // Calculate frame data pointer
    uint32_t dataOffset = mHeader->dataOffset.load(std::memory_order_acquire);
    if (dataOffset >= mMappedSize) {
        ALOGE("Invalid data offset: %u", dataOffset);
        munmap(mMappedAddr, mMappedSize);
        mMappedAddr = nullptr;
        mHeader = nullptr;
        return false;
    }
    
    mFrameData = static_cast<uint8_t*>(mMappedAddr) + dataOffset;
    
    ALOGI("Mapped shared memory: %zu bytes, data offset: %u", mMappedSize, dataOffset);
    return true;
}

void VirtualCameraFrameSource::unmapSharedMemory() {
    if (mMappedAddr != nullptr) {
        munmap(mMappedAddr, mMappedSize);
        mMappedAddr = nullptr;
        mHeader = nullptr;
        mFrameData = nullptr;
        mMappedSize = 0;
    }
    
    if (mShmFd >= 0) {
        close(mShmFd);
        mShmFd = -1;
    }
}

bool VirtualCameraFrameSource::isRendererActive() {
    std::lock_guard<std::mutex> lock(mLock);
    
    if (mHeader == nullptr) {
        return false;
    }
    
    uint32_t flags = mHeader->flags.load(std::memory_order_acquire);
    return (flags & FLAG_RENDERER_ACTIVE) != 0;
}

bool VirtualCameraFrameSource::acquireFrame(
        void* destBuffer, size_t destSize,
        uint32_t* outWidth, uint32_t* outHeight,
        uint64_t* outTimestamp) {
    
    std::lock_guard<std::mutex> lock(mLock);
    
    if (mHeader == nullptr || mFrameData == nullptr) {
        return false;
    }
    
    // Check if renderer is active
    uint32_t flags = mHeader->flags.load(std::memory_order_acquire);
    if ((flags & FLAG_RENDERER_ACTIVE) == 0) {
        return false;
    }
    
    // Read frame info
    uint32_t width = mHeader->width.load(std::memory_order_acquire);
    uint32_t height = mHeader->height.load(std::memory_order_acquire);
    uint32_t dataSize = mHeader->dataSize.load(std::memory_order_acquire);
    uint64_t timestamp = mHeader->timestamp.load(std::memory_order_acquire);
    
    // Track timestamp for logging
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
    
    return true;
}

bool VirtualCameraFrameSource::getFrameInfo(
        uint32_t* width, uint32_t* height, uint32_t* format) {
    
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
