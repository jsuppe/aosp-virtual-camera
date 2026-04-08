/*
 * VirtualCameraSocket - Unix domain socket for fd passing
 */

#define LOG_TAG "VirtualCameraSocket"

#include "VirtualCameraSocket.h"

#include <log/log.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace virtualcamera {

VirtualCameraSocket::VirtualCameraSocket() = default;

VirtualCameraSocket::~VirtualCameraSocket() {
    stop();
}

bool VirtualCameraSocket::start() {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mRunning) {
        return true;
    }

    // Create Unix domain socket
    mServerFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mServerFd < 0) {
        ALOGE("Failed to create socket: %s", strerror(errno));
        return false;
    }

    // Remove existing socket file
    unlink(SOCKET_PATH);

    // Bind to socket path
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(mServerFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ALOGE("Failed to bind socket: %s", strerror(errno));
        close(mServerFd);
        mServerFd = -1;
        return false;
    }

    // Allow apps to connect (need to chmod for SELinux)
    chmod(SOCKET_PATH, 0666);

    // Listen for connections
    if (listen(mServerFd, 1) < 0) {
        ALOGE("Failed to listen: %s", strerror(errno));
        close(mServerFd);
        mServerFd = -1;
        return false;
    }

    ALOGI("Socket server started at %s", SOCKET_PATH);

    mRunning = true;
    mAcceptThread = std::thread(&VirtualCameraSocket::acceptLoop, this);

    return true;
}

void VirtualCameraSocket::stop() {
    mRunning = false;

    if (mServerFd >= 0) {
        shutdown(mServerFd, SHUT_RDWR);
        close(mServerFd);
        mServerFd = -1;
    }

    if (mAcceptThread.joinable()) {
        mAcceptThread.join();
    }

    int fd = mSharedMemFd.exchange(-1);
    if (fd >= 0) {
        close(fd);
    }

    unlink(SOCKET_PATH);
    ALOGI("Socket server stopped");
}

void VirtualCameraSocket::acceptLoop() {
    ALOGI("Accept loop started");

    while (mRunning) {
        struct sockaddr_un clientAddr;
        socklen_t clientLen = sizeof(clientAddr);

        int clientFd = accept(mServerFd, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientFd < 0) {
            if (mRunning) {
                ALOGE("Accept failed: %s", strerror(errno));
            }
            continue;
        }

        ALOGI("Renderer connected");

        // Receive the shared memory fd
        size_t size = 0;
        int shmFd = receiveFd(clientFd, &size);
        close(clientFd);

        if (shmFd >= 0) {
            // Close old fd if any
            int oldFd = mSharedMemFd.exchange(shmFd);
            if (oldFd >= 0) {
                close(oldFd);
            }

            ALOGI("Received shared memory fd: %d, size: %zu", shmFd, size);

            if (mOnFdReceived) {
                mOnFdReceived(shmFd, size);
            }
        } else {
            ALOGE("Failed to receive fd from renderer");
        }
    }

    ALOGI("Accept loop ended");
}

int VirtualCameraSocket::receiveFd(int clientFd, size_t* outSize) {
    // Protocol: client sends size (size_t) + fd via SCM_RIGHTS

    size_t size = 0;
    struct iovec iov;
    iov.iov_base = &size;
    iov.iov_len = sizeof(size);

    // Buffer for control message (fd passing)
    char cmsgBuf[CMSG_SPACE(sizeof(int))];

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgBuf;
    msg.msg_controllen = sizeof(cmsgBuf);

    ssize_t n = recvmsg(clientFd, &msg, 0);
    if (n < 0) {
        ALOGE("recvmsg failed: %s", strerror(errno));
        return -1;
    }

    if (n != sizeof(size)) {
        ALOGE("Received unexpected size: %zd vs %zu", n, sizeof(size));
        return -1;
    }

    // Extract fd from control message
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS) {
        ALOGE("No fd in control message");
        return -1;
    }

    int fd = *((int*)CMSG_DATA(cmsg));

    if (outSize) {
        *outSize = size;
    }

    return fd;
}

}  // namespace virtualcamera
