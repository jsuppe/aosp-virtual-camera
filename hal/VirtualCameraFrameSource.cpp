/*
 * VirtualCameraFrameSource - Shared memory frame source implementation
 */

#define LOG_TAG "VirtualCameraFrameSource"

#include "VirtualCameraFrameSource.h"

#include <log/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace aidl::android::hardware::camera::provider::implementation {

VirtualCameraFrameSource::VirtualCameraFrameSource() {
    // Try to map shared memory on construction
    mapSharedMemory();
}

VirtualCameraFrameSource::~VirtualCameraFrameSource() {
    unmapSharedMemory();
}

bool VirtualCameraFrameSource::mapSharedMemory() {
    std::lock_guard<std::mutex> lock(mLock);
    
    if (mMappedAddr != nullptr) {
        return true;  // Already mapped
    }
    
    // Try to open existing shared memory
    mFd = open(SHARED_MEM_PATH, O_RDONLY);
    if (mFd < 0) {
        // Not an error - renderer may not have started yet
        return false;
    }
    
    // Get file size
    struct stat st;
    if (fstat(mFd, &st) < 0) {
        close(mFd);
        mFd = -1;
        return false;
    }
    mMappedSize = st.st_size;
    
    // Map the shared memory
    mMappedAddr = mmap(nullptr, mMappedSize, PROT_READ, MAP_SHARED, mFd, 0);
    if (mMappedAddr == MAP_FAILED) {
        ALOGE("Failed to mmap shared memory: %s", strerror(errno));
        close(mFd);
        mFd = -1;
        mMappedAddr = nullptr;
        return false;
    }
    
    // Validate header
    mHeader = static_cast<FrameHeader*>(mMappedAddr);
    if (mHeader->magic.load() != VCMF_MAGIC) {
        ALOGE("Invalid shared memory magic: 0x%x", mHeader->magic.load());
        unmapSharedMemory();
        return false;
    }
    
    if (mHeader->version.load() != VCMF_VERSION) {
        ALOGE("Unsupported shared memory version: %u", mHeader->version.load());
        unmapSharedMemory();
        return false;
    }
    
    // Get pointer to frame data
    uint32_t dataOffset = mHeader->dataOffset.load();
    if (dataOffset < sizeof(FrameHeader) || dataOffset >= mMappedSize) {
        ALOGE("Invalid data offset: %u", dataOffset);
        unmapSharedMemory();
        return false;
    }
    mFrameData = static_cast<uint8_t*>(mMappedAddr) + dataOffset;
    
    ALOGI("Mapped shared memory: %ux%u, format=%u",
          mHeader->width.load(), mHeader->height.load(), mHeader->format.load());
    
    return true;
}

void VirtualCameraFrameSource::unmapSharedMemory() {
    if (mMappedAddr != nullptr) {
        munmap(mMappedAddr, mMappedSize);
        mMappedAddr = nullptr;
        mHeader = nullptr;
        mFrameData = nullptr;
    }
    if (mFd >= 0) {
        close(mFd);
        mFd = -1;
    }
    mMappedSize = 0;
}

bool VirtualCameraFrameSource::isRendererActive() {
    // Try to map if not already
    if (mMappedAddr == nullptr) {
        if (!mapSharedMemory()) {
            return false;
        }
    }
    
    return (mHeader->flags.load() & FLAG_RENDERER_ACTIVE) != 0;
}

bool VirtualCameraFrameSource::getFrameInfo(uint32_t* width, uint32_t* height, uint32_t* format) {
    if (mMappedAddr == nullptr) {
        if (!mapSharedMemory()) {
            return false;
        }
    }
    
    if (mHeader == nullptr) {
        return false;
    }
    
    *width = mHeader->width.load();
    *height = mHeader->height.load();
    *format = mHeader->format.load();
    return true;
}

bool VirtualCameraFrameSource::acquireFrame(void* destBuffer, size_t destSize,
                                            uint32_t* outWidth, uint32_t* outHeight,
                                            uint64_t* outTimestamp) {
    std::lock_guard<std::mutex> lock(mLock);
    
    if (mMappedAddr == nullptr) {
        if (!mapSharedMemory()) {
            return false;
        }
    }
    
    if (mHeader == nullptr || mFrameData == nullptr) {
        return false;
    }
    
    // Check if new frame is available
    uint32_t flags = mHeader->flags.load();
    if (!(flags & FLAG_NEW_FRAME)) {
        return false;  // No new frame
    }
    
    // Get frame metadata
    *outWidth = mHeader->width.load();
    *outHeight = mHeader->height.load();
    *outTimestamp = mHeader->timestamp.load();
    
    uint32_t dataSize = mHeader->dataSize.load();
    if (dataSize > destSize) {
        ALOGE("Frame too large: %u > %zu", dataSize, destSize);
        return false;
    }
    
    // Copy frame data
    memcpy(destBuffer, mFrameData, dataSize);
    
    // Note: We don't clear NEW_FRAME flag since we're read-only
    // The renderer manages the flag
    
    return true;
}

}  // namespace
