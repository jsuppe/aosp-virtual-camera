/*
 * VirtualCameraRenderer - Implementation
 */

#define LOG_TAG "VirtualCameraRenderer"

#include "VirtualCameraRenderer.h"

#include <android/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <linux/ashmem.h>
#include <sys/ioctl.h>

#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace virtualcamera {

VirtualCameraRenderer::VirtualCameraRenderer() {
}

VirtualCameraRenderer::~VirtualCameraRenderer() {
    shutdown();
}

size_t VirtualCameraRenderer::calculateFrameSize(uint32_t width, uint32_t height, PixelFormat format) {
    switch (format) {
        case PixelFormat::RGBA_8888:
            return width * height * 4;
        case PixelFormat::YUV_420_888:
        case PixelFormat::IMPLEMENTATION_DEFINED:
            // Y plane + UV planes (4:2:0)
            return width * height + (width * height / 2);
        default:
            return width * height * 4;  // Default to RGBA
    }
}

bool VirtualCameraRenderer::initialize(uint32_t width, uint32_t height, PixelFormat format) {
    std::lock_guard<std::mutex> lock(mLock);
    
    if (mInitialized) {
        ALOGE("Already initialized");
        return false;
    }
    
    mWidth = width;
    mHeight = height;
    mFormat = format;
    mStride = width;  // Assuming tightly packed
    mFrameSize = calculateFrameSize(width, height, format);
    
    if (!createSharedMemory()) {
        return false;
    }
    
    // Initialize header
    mHeader->magic = VCMF_MAGIC;
    mHeader->version = VCMF_VERSION;
    mHeader->width = width;
    mHeader->height = height;
    mHeader->format = static_cast<uint32_t>(format);
    mHeader->stride = mStride;
    mHeader->frameNumber = 0;
    mHeader->timestamp = 0;
    mHeader->dataOffset = sizeof(FrameHeader);
    mHeader->dataSize = mFrameSize;
    mHeader->flags = FLAG_RENDERER_ACTIVE;
    
    mInitialized = true;
    ALOGI("Initialized: %ux%u, format=%u, frameSize=%zu", width, height, 
          static_cast<uint32_t>(format), mFrameSize);
    
    return true;
}

bool VirtualCameraRenderer::createSharedMemory() {
    // Calculate total size needed
    mMappedSize = sizeof(FrameHeader) + mFrameSize;
    
    // Create ashmem region
    mFd = open("/dev/ashmem", O_RDWR);
    if (mFd < 0) {
        ALOGE("Failed to open /dev/ashmem: %s", strerror(errno));
        return false;
    }
    
    // Set name
    if (ioctl(mFd, ASHMEM_SET_NAME, "virtual_camera_frames") < 0) {
        ALOGE("Failed to set ashmem name: %s", strerror(errno));
        close(mFd);
        mFd = -1;
        return false;
    }
    
    // Set size
    if (ioctl(mFd, ASHMEM_SET_SIZE, mMappedSize) < 0) {
        ALOGE("Failed to set ashmem size: %s", strerror(errno));
        close(mFd);
        mFd = -1;
        return false;
    }
    
    // Map the memory
    mMappedAddr = mmap(nullptr, mMappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, mFd, 0);
    if (mMappedAddr == MAP_FAILED) {
        ALOGE("Failed to mmap ashmem: %s", strerror(errno));
        close(mFd);
        mFd = -1;
        mMappedAddr = nullptr;
        return false;
    }
    
    // Set up pointers
    mHeader = static_cast<FrameHeader*>(mMappedAddr);
    mFrameData = static_cast<uint8_t*>(mMappedAddr) + sizeof(FrameHeader);
    
    // Also create a symlink so HAL can find us
    // Note: This requires appropriate permissions
    unlink(SHARED_MEM_PATH);
    char fdPath[64];
    snprintf(fdPath, sizeof(fdPath), "/proc/self/fd/%d", mFd);
    if (symlink(fdPath, SHARED_MEM_PATH) < 0) {
        ALOGE("Warning: Failed to create symlink %s: %s", SHARED_MEM_PATH, strerror(errno));
        // Continue anyway - HAL might use other discovery method
    }
    
    return true;
}

void VirtualCameraRenderer::destroySharedMemory() {
    if (mMappedAddr != nullptr) {
        // Clear renderer active flag
        if (mHeader != nullptr) {
            mHeader->flags = 0;
        }
        munmap(mMappedAddr, mMappedSize);
        mMappedAddr = nullptr;
        mHeader = nullptr;
        mFrameData = nullptr;
    }
    
    if (mFd >= 0) {
        close(mFd);
        mFd = -1;
    }
    
    unlink(SHARED_MEM_PATH);
}

void VirtualCameraRenderer::shutdown() {
    std::lock_guard<std::mutex> lock(mLock);
    
    if (!mInitialized) {
        return;
    }
    
    destroySharedMemory();
    mInitialized = false;
    mFrameNumber = 0;
    
    ALOGI("Shutdown complete");
}

void* VirtualCameraRenderer::beginFrame() {
    if (!mInitialized || mFrameData == nullptr) {
        return nullptr;
    }
    
    // Clear new frame flag while we're writing
    mHeader->flags = FLAG_RENDERER_ACTIVE;
    
    return mFrameData;
}

void VirtualCameraRenderer::endFrame(uint64_t timestamp) {
    if (!mInitialized || mHeader == nullptr) {
        return;
    }
    
    // Update metadata
    mFrameNumber++;
    mHeader->frameNumber = mFrameNumber;
    
    if (timestamp == 0) {
        auto now = std::chrono::steady_clock::now();
        timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count();
    }
    mHeader->timestamp = timestamp;
    
    // Signal new frame available
    mHeader->flags = FLAG_RENDERER_ACTIVE | FLAG_NEW_FRAME;
}

}  // namespace virtualcamera
