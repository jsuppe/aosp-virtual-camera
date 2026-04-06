/*
 * VirtualCameraWriterV2 - Zero-copy frame writer using AHardwareBuffer pool
 *
 * Renderer-side counterpart to VirtualCameraFrameSourceV2 in the HAL.
 * Allocates a SharedBufferPool, connects to the v2 socket, sends the
 * pool to the HAL, then writes frames via AHardwareBuffer_lock (CPU path)
 * or via RendererInterface (GPU path).
 *
 * This class handles the CPU path. For GPU rendering, use RendererInterface
 * directly after calling setupPool()/connectToHal().
 */
#pragma once

#include "../../../../v2-shared-memory/SharedBufferPool.h"

#include <android/hardware_buffer.h>
#include <cstdint>
#include <atomic>

namespace vcam {

class VirtualCameraWriterV2 {
public:
    VirtualCameraWriterV2();
    ~VirtualCameraWriterV2();

    /**
     * Initialize: allocate buffer pool and connect to the HAL's v2 socket.
     * Returns true if the HAL acknowledged the connection.
     */
    bool initialize(uint32_t width, uint32_t height,
                    int32_t format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                    int32_t bufferCount = 4);

    void shutdown();

    /**
     * Begin a frame: acquire a free buffer and lock it for CPU write.
     * Returns pointer to pixel data, or nullptr if no buffer available.
     * Call endFrame() when done writing.
     */
    uint8_t* beginFrame();

    /**
     * Finish the current frame and submit it to the HAL via the control ring.
     */
    void endFrame();

    /**
     * Check if the HAL has requested a different format/resolution.
     * If returns true, the renderer should call shutdown(), then
     * initialize() again with the new parameters.
     */
    struct FormatRequest {
        int32_t format;
        int32_t width;
        int32_t height;
        uint64_t usage;
    };
    bool checkNegotiation(FormatRequest* out);

    // Accessors
    uint32_t getWidth() const { return mWidth; }
    uint32_t getHeight() const { return mHeight; }
    int32_t getFormat() const { return mFormat; }
    bool isInitialized() const { return mInitialized; }

    // Access the pool directly (for GPU path via RendererInterface)
    virtualcamera::SharedBufferPool* getPool() { return &mPool; }

private:
    bool connectToHal();
    static int sendFdViaSCM(int socketFd, int fd);

    virtualcamera::SharedBufferPool mPool;

    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    int32_t mFormat = 0;
    int mSocketFd = -1;
    bool mInitialized = false;

    // Current frame state
    int mCurrentBufferIndex = -1;
    void* mLockedPtr = nullptr;
    int64_t mCurrentTimestampNs = 0;

    // Negotiation tracking
    uint32_t mLastNegotiationSeq = 0;

    static constexpr const char* SOCKET_PATH_V2 =
        "/data/local/tmp/virtual_camera_v2.sock";
};

}  // namespace vcam
