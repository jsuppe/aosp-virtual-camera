/**
 * VirtualCameraWriter - Write frames to shared memory for HAL
 * 
 * This allows the app to act as a "camera renderer" - frames written here
 * appear as camera output in the virtual camera HAL.
 */

#include "virtual_camera_writer.h"
#include <android/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <chrono>

#define LOG_TAG "VCamWriter"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace vcam {

VirtualCameraWriter::VirtualCameraWriter() = default;

VirtualCameraWriter::~VirtualCameraWriter() {
    shutdown();
}

bool VirtualCameraWriter::initialize(uint32_t width, uint32_t height) {
    if (mMappedAddr != nullptr) {
        LOGE("Already initialized");
        return false;
    }
    
    mWidth = width;
    mHeight = height;
    
    // Calculate required size: header + RGBA frame data
    size_t dataOffset = sizeof(FrameHeader);
    // Align to 4KB
    dataOffset = (dataOffset + 4095) & ~4095;
    
    size_t frameSize = width * height * 4;  // RGBA
    mMappedSize = dataOffset + frameSize;
    
    // Create shared memory file
    const char* path = "/data/local/tmp/virtual_camera_shm";
    
    mFd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (mFd < 0) {
        LOGE("Failed to create shared memory: %s", strerror(errno));
        return false;
    }
    
    // Set file size
    if (ftruncate(mFd, mMappedSize) < 0) {
        LOGE("Failed to set shared memory size: %s", strerror(errno));
        close(mFd);
        mFd = -1;
        return false;
    }
    
    // Map the file
    mMappedAddr = mmap(nullptr, mMappedSize, PROT_READ | PROT_WRITE, 
                       MAP_SHARED, mFd, 0);
    if (mMappedAddr == MAP_FAILED) {
        LOGE("Failed to mmap: %s", strerror(errno));
        mMappedAddr = nullptr;
        close(mFd);
        mFd = -1;
        return false;
    }
    
    // Initialize header
    mHeader = static_cast<FrameHeader*>(mMappedAddr);
    mHeader->magic.store(VCMF_MAGIC, std::memory_order_release);
    mHeader->version.store(VCMF_VERSION, std::memory_order_release);
    mHeader->width.store(width, std::memory_order_release);
    mHeader->height.store(height, std::memory_order_release);
    mHeader->format.store(FORMAT_RGBA_8888, std::memory_order_release);
    mHeader->stride.store(width * 4, std::memory_order_release);
    mHeader->frameNumber.store(0, std::memory_order_release);
    mHeader->timestamp.store(0, std::memory_order_release);
    mHeader->dataOffset.store(dataOffset, std::memory_order_release);
    mHeader->dataSize.store(frameSize, std::memory_order_release);
    mHeader->flags.store(FLAG_RENDERER_ACTIVE, std::memory_order_release);
    
    mFrameData = static_cast<uint8_t*>(mMappedAddr) + dataOffset;
    memset(mFrameData, 0, frameSize);
    
    LOGI("Initialized: %ux%u, %zu bytes at %s", width, height, mMappedSize, path);
    return true;
}

void VirtualCameraWriter::shutdown() {
    if (mHeader != nullptr) {
        mHeader->flags.fetch_and(~FLAG_RENDERER_ACTIVE, std::memory_order_release);
    }
    
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
    
    mWidth = 0;
    mHeight = 0;
    mMappedSize = 0;
    LOGI("Shutdown complete");
}

void VirtualCameraWriter::writeFrame(const uint8_t* rgbaData, size_t size) {
    if (mFrameData == nullptr || mHeader == nullptr) {
        return;
    }
    
    size_t expectedSize = mWidth * mHeight * 4;
    if (size != expectedSize) {
        LOGE("Frame size mismatch: %zu vs expected %zu", size, expectedSize);
        return;
    }
    
    // Copy frame data
    memcpy(mFrameData, rgbaData, size);
    
    // Update header
    auto now = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    
    mFrameNumber++;
    mHeader->frameNumber.store(mFrameNumber, std::memory_order_release);
    mHeader->timestamp.store(static_cast<uint64_t>(ns), std::memory_order_release);
    mHeader->flags.fetch_or(FLAG_NEW_FRAME, std::memory_order_release);
}

}  // namespace vcam
