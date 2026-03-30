/*
 * VirtualCameraSocket - Unix domain socket for fd passing
 * 
 * Renderer connects and sends its ashmem/memfd file descriptor.
 * HAL receives it and maps for reading frames.
 */

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace aidl::android::hardware::camera::provider::implementation {

class VirtualCameraSocket {
public:
    VirtualCameraSocket();
    ~VirtualCameraSocket();
    
    // Start listening for renderer connections
    bool start();
    
    // Stop the socket server
    void stop();
    
    // Get the current shared memory fd (-1 if none)
    int getSharedMemoryFd() const { return mSharedMemFd.load(); }
    
    // Check if a renderer is connected
    bool isRendererConnected() const { return mSharedMemFd.load() >= 0; }
    
    // Callback when new fd is received
    using OnFdReceivedCallback = std::function<void(int fd, size_t size)>;
    void setOnFdReceived(OnFdReceivedCallback callback) { mOnFdReceived = callback; }

private:
    void acceptLoop();
    int receiveFd(int clientFd, size_t* outSize);
    
    int mServerFd = -1;
    std::atomic<int> mSharedMemFd{-1};
    std::atomic<bool> mRunning{false};
    std::thread mAcceptThread;
    std::mutex mMutex;
    OnFdReceivedCallback mOnFdReceived;
    
    static constexpr const char* SOCKET_PATH = "/data/local/tmp/virtual_camera.sock";
};

}  // namespace
