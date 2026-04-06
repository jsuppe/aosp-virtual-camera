/*
 * VirtualCameraFrameSourceV2 - Zero-copy frame source using AHardwareBuffer pool
 *
 * Receives a SharedBufferPool from the renderer via Unix socket, then
 * uses HalInterface for lock-free, zero-copy frame acquisition.
 *
 * Socket protocol (v2):
 *   1. Renderer connects
 *   2. Sends SetupHeader (version=2, buffer count, dimensions, format)
 *   3. Sends control ring fd via SCM_RIGHTS
 *   4. Sends each AHardwareBuffer via AHardwareBuffer_sendHandleToUnixSocket
 *   5. HAL attaches to pool, begins consuming from the control ring
 */
#pragma once

#include "SharedBufferPool.h"
#include "HalInterface.h"

#include <android/hardware_buffer.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace aidl::android::hardware::camera::provider::implementation {

class VirtualCameraFrameSourceV2 {
public:
    // Wire-format header sent by the renderer at connection time
    struct SetupHeader {
        uint32_t magic;         // 0x56434D32 = "VCM2"
        uint32_t version;       // 2
        int32_t  width;
        int32_t  height;
        int32_t  format;        // AHARDWAREBUFFER_FORMAT_*
        int32_t  bufferCount;
        uint64_t usage;
    };

    static constexpr uint32_t VCM2_MAGIC = 0x56434D32;
    static constexpr uint32_t VCM2_VERSION = 2;

    VirtualCameraFrameSourceV2();
    ~VirtualCameraFrameSourceV2();

    // Start the socket server (separate path from v1)
    bool start();
    void stop();

    // Is a renderer connected with a valid buffer pool?
    bool isActive() const {
        return mActive.load(std::memory_order_acquire);
    }

    // Access the HalInterface for frame acquisition.
    // Returns nullptr if no renderer is connected.
    virtualcamera::HalInterface* getHalInterface() {
        std::lock_guard<std::mutex> lock(mLock);
        if (!mActive.load(std::memory_order_relaxed)) return nullptr;
        return &mHalInterface;
    }

    // Get buffer pool config (dimensions, format)
    bool getConfig(int32_t* width, int32_t* height, int32_t* format) const {
        if (!mActive.load(std::memory_order_acquire)) return false;
        const auto& cfg = mPool.getConfig();
        if (width) *width = cfg.width;
        if (height) *height = cfg.height;
        if (format) *format = cfg.format;
        return true;
    }

    /**
     * Publish a format/resolution request to the renderer via the
     * shared negotiation block. The renderer polls this on each
     * beginFrame() and can choose to reconnect with a new pool
     * in the requested format.
     *
     * Called by VirtualCameraSession::configureStreams().
     */
    void requestFormat(int32_t format, int32_t width, int32_t height,
                       uint64_t usage) {
        std::lock_guard<std::mutex> lock(mLock);
        auto* ring = mPool.getControlRing();
        if (!ring) return;

        ring->negotiation.format.store(format, std::memory_order_relaxed);
        ring->negotiation.width.store(width, std::memory_order_relaxed);
        ring->negotiation.height.store(height, std::memory_order_relaxed);
        ring->negotiation.usage.store(usage, std::memory_order_relaxed);
        // Bump seq last with release so renderer sees consistent values
        ring->negotiation.seq.fetch_add(1, std::memory_order_release);
    }

private:
    void acceptLoop();
    bool handleConnection(int clientFd);
    int receiveFdViaSCM(int clientFd);

    int mServerFd = -1;
    std::atomic<bool> mRunning{false};
    std::atomic<bool> mActive{false};
    std::thread mAcceptThread;
    std::mutex mLock;

    virtualcamera::SharedBufferPool mPool;
    virtualcamera::HalInterface mHalInterface;

    static constexpr const char* SOCKET_PATH_V2 =
        "/data/local/tmp/virtual_camera_v2.sock";
};

}  // namespace
