/**
 * VirtualCameraWriterV2 - Zero-copy frame writer implementation
 */

#include "virtual_camera_writer_v2.h"

#include <android/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <time.h>

#define LOG_TAG "VCamWriterV2"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace vcam {

VirtualCameraWriterV2::VirtualCameraWriterV2() = default;

VirtualCameraWriterV2::~VirtualCameraWriterV2() {
    shutdown();
}

bool VirtualCameraWriterV2::initialize(uint32_t width, uint32_t height,
                                       int32_t format, int32_t bufferCount) {
    if (mInitialized) {
        LOGE("Already initialized");
        return false;
    }

    mWidth = width;
    mHeight = height;
    mFormat = format;

    // Check for Cuttlefish/virtual GPU where cross-process AHB CPU
    // access doesn't work (virtio-gpu doesn't sync CPU buffer contents)
    FILE* fp = fopen("/proc/device-tree/compatible", "r");
    if (!fp) fp = fopen("/sys/class/dmi/id/product_name", "r");
    if (fp) {
        char buf[256] = {};
        fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        if (strstr(buf, "Cuttlefish") || strstr(buf, "vsoc")) {
            LOGE("Cuttlefish detected — v2 cross-process AHB CPU access "
                 "not supported by virtio-gpu, falling back to v1");
            return false;
        }
    }

    // Allocate the buffer pool
    uint64_t usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN
                   | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN
                   | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
                   | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;

    if (!mPool.allocate(width, height, format, bufferCount, usage)) {
        LOGE("Failed to allocate buffer pool (%ux%u, format=%d, count=%d)",
             width, height, format, bufferCount);
        return false;
    }

    LOGI("Buffer pool allocated: %ux%u, format=%d, %d buffers",
         width, height, format, bufferCount);

    // Connect to the HAL and send the pool
    if (!connectToHal()) {
        LOGE("Failed to connect to HAL");
        mPool.release();
        return false;
    }

    mInitialized = true;
    LOGI("V2 writer initialized successfully");
    return true;
}

void VirtualCameraWriterV2::shutdown() {
    // Unlock any in-progress frame
    if (mLockedPtr && mCurrentBufferIndex >= 0) {
        AHardwareBuffer_unlock(mPool.getBuffer(mCurrentBufferIndex), nullptr);
        mPool.releaseBuffer(mCurrentBufferIndex);
        mLockedPtr = nullptr;
        mCurrentBufferIndex = -1;
    }

    if (mSocketFd >= 0) {
        close(mSocketFd);
        mSocketFd = -1;
    }

    mPool.release();
    mInitialized = false;
    mWidth = 0;
    mHeight = 0;
    mFormat = 0;
    mLastNegotiationSeq = 0;

    LOGI("V2 writer shut down");
}

bool VirtualCameraWriterV2::connectToHal() {
    // Connect to the v2 Unix socket
    mSocketFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mSocketFd < 0) {
        LOGE("Failed to create socket: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH_V2, sizeof(addr.sun_path) - 1);

    if (connect(mSocketFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("Failed to connect to %s: %s", SOCKET_PATH_V2, strerror(errno));
        close(mSocketFd);
        mSocketFd = -1;
        return false;
    }

    LOGI("Connected to HAL v2 socket");

    // Step 1: Send setup header
    struct SetupHeader {
        uint32_t magic;
        uint32_t version;
        int32_t  width;
        int32_t  height;
        int32_t  format;
        int32_t  bufferCount;
        uint64_t usage;
    };

    SetupHeader header;
    header.magic = 0x56434D32;  // "VCM2"
    header.version = 2;
    header.width = static_cast<int32_t>(mWidth);
    header.height = static_cast<int32_t>(mHeight);
    header.format = mFormat;
    header.bufferCount = mPool.getBufferCount();
    header.usage = mPool.getConfig().usage;

    if (send(mSocketFd, &header, sizeof(header), 0) != sizeof(header)) {
        LOGE("Failed to send setup header");
        close(mSocketFd);
        mSocketFd = -1;
        return false;
    }

    // Step 2: Send control ring fd via SCM_RIGHTS
    if (sendFdViaSCM(mSocketFd, mPool.getControlRingFd()) < 0) {
        LOGE("Failed to send control ring fd");
        close(mSocketFd);
        mSocketFd = -1;
        return false;
    }

    LOGI("Sent control ring fd");

    // Step 3: Send AHardwareBuffers
    if (!mPool.sendBuffersTo(mSocketFd)) {
        LOGE("Failed to send AHardwareBuffers");
        close(mSocketFd);
        mSocketFd = -1;
        return false;
    }

    LOGI("Sent %d AHardwareBuffers", mPool.getBufferCount());

    // Step 4: Wait for ack from HAL
    uint8_t ack = 0;
    ssize_t n = recv(mSocketFd, &ack, 1, 0);
    if (n != 1 || ack != 1) {
        LOGE("Failed to receive ack from HAL (n=%zd, ack=%d)", n, ack);
        close(mSocketFd);
        mSocketFd = -1;
        return false;
    }

    LOGI("HAL acknowledged, v2 pipeline active");
    return true;
}

int VirtualCameraWriterV2::sendFdViaSCM(int socketFd, int fd) {
    uint8_t dummy = 0;
    struct iovec iov;
    iov.iov_base = &dummy;
    iov.iov_len = 1;

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
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    ssize_t n = sendmsg(socketFd, &msg, 0);
    return (n >= 0) ? 0 : -1;
}

uint8_t* VirtualCameraWriterV2::beginFrame() {
    if (!mInitialized) return nullptr;

    // Drain release ring to reclaim buffers the HAL is done with
    auto* ring = mPool.getControlRing();
    if (ring) {
        virtualcamera::ControlRing::ReleaseSlot rslot;
        while (ring->tryReadRelease(&rslot)) {
            // bufferState already set to 0 by tryWriteRelease
        }
    }

    // Acquire a free buffer
    int bufIdx = mPool.acquireFreeBuffer();
    if (bufIdx < 0) {
        return nullptr;  // All buffers in flight
    }

    AHardwareBuffer* buffer = mPool.getBuffer(bufIdx);
    if (!buffer) {
        mPool.releaseBuffer(bufIdx);
        return nullptr;
    }

    // Lock for CPU write
    void* ptr = nullptr;
    int result = AHardwareBuffer_lock(
        buffer,
        AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
        -1,       // no fence
        nullptr,  // full rect
        &ptr);

    if (result != 0 || !ptr) {
        LOGE("Failed to lock buffer %d for writing", bufIdx);
        mPool.releaseBuffer(bufIdx);
        return nullptr;
    }

    mCurrentBufferIndex = bufIdx;
    mLockedPtr = ptr;

    // Capture timestamp
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    mCurrentTimestampNs = ts.tv_sec * 1000000000LL + ts.tv_nsec;

    return static_cast<uint8_t*>(ptr);
}

void VirtualCameraWriterV2::endFrame() {
    if (mCurrentBufferIndex < 0 || !mLockedPtr) return;

    AHardwareBuffer* buffer = mPool.getBuffer(mCurrentBufferIndex);

    // Unlock the buffer
    AHardwareBuffer_unlock(buffer, nullptr);
    mLockedPtr = nullptr;

    // Submit to the control ring (no fence for CPU writes)
    auto* ring = mPool.getControlRing();
    if (ring) {
        if (!ring->tryWrite(mCurrentBufferIndex, -1, mCurrentTimestampNs)) {
            // Ring full — release buffer back
            mPool.releaseBuffer(mCurrentBufferIndex);
        }
    } else {
        mPool.releaseBuffer(mCurrentBufferIndex);
    }

    mCurrentBufferIndex = -1;
}

bool VirtualCameraWriterV2::checkNegotiation(FormatRequest* out) {
    if (!mInitialized) return false;

    auto* ring = mPool.getControlRing();
    if (!ring) return false;

    uint32_t seq = ring->negotiation.seq.load(std::memory_order_acquire);
    if (seq == mLastNegotiationSeq) return false;

    mLastNegotiationSeq = seq;
    if (out) {
        out->format = ring->negotiation.format.load(std::memory_order_relaxed);
        out->width = ring->negotiation.width.load(std::memory_order_relaxed);
        out->height = ring->negotiation.height.load(std::memory_order_relaxed);
        out->usage = ring->negotiation.usage.load(std::memory_order_relaxed);
    }
    return true;
}

}  // namespace vcam
