/*
 * VirtualCameraClient - Renderer-side client using ashmem
 */

#include "VirtualCameraClient.h"

#include <android/log.h>
#include <android/sharedmem.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>

#define LOG_TAG "VCamClient"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace vcam {

VirtualCameraClient::VirtualCameraClient() = default;

VirtualCameraClient::~VirtualCameraClient() {
    shutdown();
}

bool VirtualCameraClient::initialize(uint32_t width, uint32_t height) {
    if (mMappedAddr != nullptr) {
        LOGE("Already initialized");
        return false;
    }
    
    mWidth = width;
    mHeight = height;
    
    if (!createSharedMemory()) {
        return false;
    }
    
    if (!connectToHal()) {
        // Clean up shared memory
        munmap(mMappedAddr, mMappedSize);
        mMappedAddr = nullptr;
        close(mShmFd);
        mShmFd = -1;
        return false;
    }
    
    mConnected = true;
    LOGI("Initialized: %ux%u, connected to HAL", width, height);
    return true;
}

bool VirtualCameraClient::createSharedMemory() {
    // Calculate required size: header + RGBA frame data
    size_t dataOffset = sizeof(FrameHeader);
    // Align to 4KB
    dataOffset = (dataOffset + 4095) & ~4095;
    
    size_t frameSize = mWidth * mHeight * 4;  // RGBA
    mMappedSize = dataOffset + frameSize;
    
    // Create ashmem region
    mShmFd = ASharedMemory_create("virtual_camera_frame", mMappedSize);
    if (mShmFd < 0) {
        LOGE("Failed to create shared memory: %s", strerror(errno));
        return false;
    }
    
    // Map it
    mMappedAddr = mmap(nullptr, mMappedSize, PROT_READ | PROT_WRITE,
                       MAP_SHARED, mShmFd, 0);
    if (mMappedAddr == MAP_FAILED) {
        LOGE("Failed to mmap: %s", strerror(errno));
        mMappedAddr = nullptr;
        close(mShmFd);
        mShmFd = -1;
        return false;
    }
    
    // Initialize header
    mHeader = static_cast<FrameHeader*>(mMappedAddr);
    mHeader->magic.store(VCMF_MAGIC, std::memory_order_release);
    mHeader->version.store(VCMF_VERSION, std::memory_order_release);
    mHeader->width.store(mWidth, std::memory_order_release);
    mHeader->height.store(mHeight, std::memory_order_release);
    mHeader->format.store(FORMAT_RGBA_8888, std::memory_order_release);
    mHeader->stride.store(mWidth * 4, std::memory_order_release);
    mHeader->frameNumber.store(0, std::memory_order_release);
    mHeader->timestamp.store(0, std::memory_order_release);
    mHeader->dataOffset.store(dataOffset, std::memory_order_release);
    mHeader->dataSize.store(frameSize, std::memory_order_release);
    mHeader->flags.store(FLAG_RENDERER_ACTIVE, std::memory_order_release);
    
    mFrameData = static_cast<uint8_t*>(mMappedAddr) + dataOffset;
    memset(mFrameData, 0, frameSize);
    
    LOGI("Created shared memory: %zu bytes, fd=%d", mMappedSize, mShmFd);
    return true;
}

bool VirtualCameraClient::connectToHal() {
    // Connect to HAL's Unix socket
    int sockFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockFd < 0) {
        LOGE("Failed to create socket: %s", strerror(errno));
        return false;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sockFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("Failed to connect to HAL socket: %s", strerror(errno));
        close(sockFd);
        return false;
    }
    
    LOGI("Connected to HAL socket");
    
    // Send shared memory size + fd
    struct iovec iov;
    iov.iov_base = &mMappedSize;
    iov.iov_len = sizeof(mMappedSize);
    
    char cmsgBuf[CMSG_SPACE(sizeof(int))];
    
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgBuf;
    msg.msg_controllen = sizeof(cmsgBuf);
    
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int*)CMSG_DATA(cmsg)) = mShmFd;
    
    ssize_t n = sendmsg(sockFd, &msg, 0);
    close(sockFd);
    
    if (n < 0) {
        LOGE("Failed to send fd: %s", strerror(errno));
        return false;
    }
    
    LOGI("Sent shared memory fd to HAL");
    return true;
}

void VirtualCameraClient::shutdown() {
    if (mHeader != nullptr) {
        mHeader->flags.fetch_and(~FLAG_RENDERER_ACTIVE, std::memory_order_release);
    }
    
    if (mMappedAddr != nullptr) {
        munmap(mMappedAddr, mMappedSize);
        mMappedAddr = nullptr;
        mHeader = nullptr;
        mFrameData = nullptr;
    }
    
    if (mShmFd >= 0) {
        close(mShmFd);
        mShmFd = -1;
    }
    
    mConnected = false;
    mWidth = 0;
    mHeight = 0;
    mMappedSize = 0;
    LOGI("Shutdown complete");
}

void VirtualCameraClient::writeFrame(const uint8_t* rgbaData, size_t size) {
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
}

}  // namespace vcam
