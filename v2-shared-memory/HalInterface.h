/*
 * HalInterface - HAL side of zero-copy virtual camera
 *
 * Reads frames from shared buffer pool with minimal latency.
 * Releases buffers back to the renderer via the release ring.
 */
#pragma once

#include "SharedBufferPool.h"
#include <android/sync.h>
#include <poll.h>
#include <unistd.h>

namespace virtualcamera {

/**
 * HAL-side interface for consuming frames.
 *
 * Usage:
 *   HalInterface hal;
 *   hal.initialize(pool);
 *
 *   // In capture request processing:
 *   AcquiredFrame frame = hal.acquireLatestFrame(timeoutMs);
 *   if (frame.valid()) {
 *       // Use frame.buffer directly — it's a GPU-rendered AHardwareBuffer
 *       hal.releaseFrame(frame);
 *   }
 */
class HalInterface {
public:
    struct AcquiredFrame {
        int bufferIndex = -1;
        AHardwareBuffer* buffer = nullptr;
        int64_t timestampNs = 0;

        bool valid() const { return bufferIndex >= 0; }
    };

    HalInterface() = default;
    ~HalInterface() = default;

    /**
     * Initialize with attached buffer pool.
     */
    bool initialize(SharedBufferPool* pool) {
        mPool = pool;
        mRing = pool->getControlRing();
        return mRing != nullptr;
    }

    /**
     * Check if frames are available.
     */
    bool hasFrame() const {
        return mRing && mRing->available() > 0;
    }

    /**
     * Acquire the latest frame, skipping older ones.
     * Waits up to timeoutMs for a frame.
     *
     * This gives lowest latency by always using newest frame.
     * Skipped frames are released back to the renderer immediately.
     */
    AcquiredFrame acquireLatestFrame(int timeoutMs = 0) {
        AcquiredFrame result;
        if (!mRing) return result;

        ControlRing::Slot slot;
        ControlRing::Slot latestSlot;
        bool hasSlot = false;

        // Drain ring, keeping only latest
        while (mRing->tryRead(&slot)) {
            // If we had a previous slot, release its buffer back
            if (hasSlot) {
                if (latestSlot.fenceFd >= 0) {
                    close(latestSlot.fenceFd);
                }
                // Release skipped buffer back to renderer via release ring
                mRing->tryWriteRelease(latestSlot.bufferIndex);
                mDroppedFrames++;
            }
            latestSlot = slot;
            hasSlot = true;
        }

        // If no frame yet, wait
        if (!hasSlot && timeoutMs > 0) {
            int waited = 0;
            while (waited < timeoutMs) {
                if (mRing->tryRead(&latestSlot)) {
                    hasSlot = true;
                    break;
                }
                usleep(100);  // 0.1ms
                waited += 1;
            }
        }

        if (!hasSlot) return result;

        // Wait for GPU fence before accessing buffer
        if (latestSlot.fenceFd >= 0) {
            sync_wait(latestSlot.fenceFd, 1000);  // 1 second timeout
            close(latestSlot.fenceFd);
        }

        result.bufferIndex = latestSlot.bufferIndex;
        result.buffer = mPool->getBuffer(latestSlot.bufferIndex);
        result.timestampNs = latestSlot.timestampNs;
        mFrameCount++;

        return result;
    }

    /**
     * Acquire next frame in order (for recording use cases).
     * Preserves frame order, doesn't skip.
     */
    AcquiredFrame acquireNextFrame(int timeoutMs = 0) {
        AcquiredFrame result;
        if (!mRing) return result;

        ControlRing::Slot slot;

        if (!mRing->tryRead(&slot)) {
            if (timeoutMs <= 0) return result;

            int waited = 0;
            while (waited < timeoutMs) {
                if (mRing->tryRead(&slot)) break;
                usleep(1000);  // 1ms
                waited++;
            }

            if (waited >= timeoutMs) return result;
        }

        // Wait for GPU fence
        if (slot.fenceFd >= 0) {
            sync_wait(slot.fenceFd, 1000);
            close(slot.fenceFd);
        }

        result.bufferIndex = slot.bufferIndex;
        result.buffer = mPool->getBuffer(slot.bufferIndex);
        result.timestampNs = slot.timestampNs;
        mFrameCount++;

        return result;
    }

    /**
     * Release frame back to renderer via the release ring.
     * The renderer will pick this up and mark the buffer as free.
     */
    void releaseFrame(const AcquiredFrame& frame) {
        if (frame.valid() && mRing) {
            mRing->tryWriteRelease(frame.bufferIndex);
        }
    }

    /**
     * Get statistics.
     */
    uint32_t getDroppedFrameCount() const { return mDroppedFrames; }
    uint32_t getFrameCount() const { return mFrameCount; }

private:
    SharedBufferPool* mPool = nullptr;
    ControlRing* mRing = nullptr;

    uint32_t mDroppedFrames = 0;
    uint32_t mFrameCount = 0;
};

}  // namespace virtualcamera
