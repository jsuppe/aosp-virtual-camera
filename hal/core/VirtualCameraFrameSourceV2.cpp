/*
 * VirtualCameraFrameSourceV2 - Zero-copy frame source implementation
 */

#define LOG_TAG "VCamFrameSourceV2"

#include "VirtualCameraFrameSourceV2.h"

#include <log/log.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace virtualcamera {

VirtualCameraFrameSourceV2::VirtualCameraFrameSourceV2() {
    ALOGI("VirtualCameraFrameSourceV2 created");
}

VirtualCameraFrameSourceV2::~VirtualCameraFrameSourceV2() {
    stop();
    ALOGI("VirtualCameraFrameSourceV2 destroyed");
}

bool VirtualCameraFrameSourceV2::start() {
    if (mRunning.load()) return true;

    mServerFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mServerFd < 0) {
        ALOGE("Failed to create v2 socket: %s", strerror(errno));
        return false;
    }

    unlink(SOCKET_PATH_V2);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH_V2, sizeof(addr.sun_path) - 1);

    if (bind(mServerFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ALOGE("Failed to bind v2 socket: %s", strerror(errno));
        close(mServerFd);
        mServerFd = -1;
        return false;
    }

    chmod(SOCKET_PATH_V2, 0666);

    if (listen(mServerFd, 1) < 0) {
        ALOGE("Failed to listen on v2 socket: %s", strerror(errno));
        close(mServerFd);
        mServerFd = -1;
        return false;
    }

    ALOGI("V2 socket server started at %s", SOCKET_PATH_V2);

    mRunning = true;
    mAcceptThread = std::thread(&VirtualCameraFrameSourceV2::acceptLoop, this);

    return true;
}

void VirtualCameraFrameSourceV2::stop() {
    mRunning = false;
    mActive = false;

    if (mServerFd >= 0) {
        shutdown(mServerFd, SHUT_RDWR);
        close(mServerFd);
        mServerFd = -1;
    }

    if (mAcceptThread.joinable()) {
        mAcceptThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(mLock);
        mPool.release();
    }

    unlink(SOCKET_PATH_V2);
    ALOGI("V2 socket server stopped");
}

void VirtualCameraFrameSourceV2::acceptLoop() {
    ALOGI("V2 accept loop started");

    while (mRunning) {
        struct sockaddr_un clientAddr;
        socklen_t clientLen = sizeof(clientAddr);

        int clientFd = accept(mServerFd, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientFd < 0) {
            if (mRunning) {
                ALOGE("V2 accept failed: %s", strerror(errno));
            }
            continue;
        }

        ALOGI("V2 renderer connected");

        if (handleConnection(clientFd)) {
            ALOGI("V2 buffer pool setup complete");
        } else {
            ALOGE("V2 buffer pool setup failed");
        }

        close(clientFd);
    }

    ALOGI("V2 accept loop ended");
}

bool VirtualCameraFrameSourceV2::handleConnection(int clientFd) {
    // Step 1: Read the setup header
    SetupHeader header;
    ssize_t n = recv(clientFd, &header, sizeof(header), MSG_WAITALL);
    if (n != sizeof(header)) {
        ALOGE("Failed to read setup header: got %zd bytes", n);
        return false;
    }

    if (header.magic != VCM2_MAGIC || header.version != VCM2_VERSION) {
        ALOGE("Invalid v2 header: magic=0x%08x version=%u",
              header.magic, header.version);
        return false;
    }

    if (header.bufferCount <= 0 || header.bufferCount > MAX_BUFFERS) {
        ALOGE("Invalid buffer count: %d", header.bufferCount);
        return false;
    }

    ALOGI("V2 setup: %dx%d format=%d buffers=%d",
          header.width, header.height, header.format, header.bufferCount);

    // Step 2: Receive control ring fd via SCM_RIGHTS
    int controlRingFd = receiveFdViaSCM(clientFd);
    if (controlRingFd < 0) {
        ALOGE("Failed to receive control ring fd");
        return false;
    }

    ALOGI("Received control ring fd: %d", controlRingFd);

    // Step 3: Set up the pool
    std::lock_guard<std::mutex> lock(mLock);

    // Tear down old pool if any
    mActive = false;
    mPool.release();

    // Attach control ring
    if (!mPool.attachControlRing(controlRingFd)) {
        ALOGE("Failed to attach control ring");
        close(controlRingFd);
        return false;
    }
    close(controlRingFd);  // pool dup'd it

    // Step 4: Receive AHardwareBuffers
    BufferPoolConfig config;
    config.width = header.width;
    config.height = header.height;
    config.format = header.format;
    config.bufferCount = header.bufferCount;
    config.usage = header.usage;

    if (!mPool.recvBuffersFrom(clientFd, header.bufferCount, config)) {
        ALOGE("Failed to receive AHardwareBuffers");
        mPool.release();
        return false;
    }

    ALOGI("Received %d AHardwareBuffers", header.bufferCount);

    // Step 5: Initialize the HalInterface
    if (!mHalInterface.initialize(&mPool)) {
        ALOGE("Failed to initialize HalInterface");
        mPool.release();
        return false;
    }

    mActive.store(true, std::memory_order_release);

    // Send an ack byte so the renderer knows we're ready
    uint8_t ack = 1;
    send(clientFd, &ack, 1, 0);

    ALOGI("V2 zero-copy pipeline active: %dx%d, %d buffers",
          header.width, header.height, header.bufferCount);

    return true;
}

int VirtualCameraFrameSourceV2::receiveFdViaSCM(int clientFd) {
    // Receive a single fd via SCM_RIGHTS. The sender writes a dummy
    // byte as the iov payload (required for ancillary data).

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

    ssize_t n = recvmsg(clientFd, &msg, 0);
    if (n < 0) {
        ALOGE("recvmsg for fd failed: %s", strerror(errno));
        return -1;
    }

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS) {
        ALOGE("No fd in SCM_RIGHTS message");
        return -1;
    }

    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd;
}

}  // namespace virtualcamera
