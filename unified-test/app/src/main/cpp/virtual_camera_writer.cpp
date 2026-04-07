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
#include <sys/socket.h>
#include <sys/un.h>
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
    
    // Open shared memory file (pre-created by shell/HAL)
    const char* path = "/data/local/tmp/virtual_camera_shm";
    
    // First try O_RDWR only (existing file)
    mFd = open(path, O_RDWR);
    if (mFd < 0) {
        // Try creating it (may fail if no write permission to dir)
        mFd = open(path, O_RDWR | O_CREAT, 0666);
        if (mFd < 0) {
            LOGE("Failed to open shared memory: %s", strerror(errno));
            return false;
        }
    }
    
    // Set file size (may fail if not owner, that's OK if file is already sized)
    if (ftruncate(mFd, mMappedSize) < 0) {
        // Check if file is already large enough
        struct stat st;
        if (fstat(mFd, &st) == 0 && st.st_size >= (off_t)mMappedSize) {
            LOGI("File already sized, using existing");
        } else {
            LOGE("Failed to set shared memory size: %s (file size: %ld, need: %zu)", 
                 strerror(errno), st.st_size, mMappedSize);
            close(mFd);
            mFd = -1;
            return false;
        }
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

    // Send fd to HAL via Unix socket (SCM_RIGHTS)
    if (!sendFdToHal()) {
        LOGE("Failed to send fd to HAL (will retry on HAL connection)");
        // Not fatal — HAL might not be ready yet
    }

    return true;
}

bool VirtualCameraWriter::sendFdToHal() {
    const char* socketPath = "/data/local/tmp/virtual_camera.sock";

    int sockFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockFd < 0) {
        LOGE("Failed to create socket: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath, sizeof(addr.sun_path) - 1);

    if (connect(sockFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("Failed to connect to HAL socket: %s", strerror(errno));
        close(sockFd);
        return false;
    }

    LOGI("Connected to HAL socket");

    // Send: size (size_t) as iov data + fd via SCM_RIGHTS
    size_t size = mMappedSize;
    struct iovec iov;
    iov.iov_base = &size;
    iov.iov_len = sizeof(size);

    char cmsgBuf[CMSG_SPACE(sizeof(int))];
    memset(cmsgBuf, 0, sizeof(cmsgBuf));

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
    *((int*)CMSG_DATA(cmsg)) = mFd;

    ssize_t sent = sendmsg(sockFd, &msg, 0);
    close(sockFd);

    if (sent < 0) {
        LOGE("sendmsg failed: %s", strerror(errno));
        return false;
    }

    LOGI("Sent fd to HAL (size=%zu)", size);
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
