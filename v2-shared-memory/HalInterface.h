/*
 * HalInterface - HAL side of zero-copy virtual camera
 * 
 * Reads frames from shared buffer pool with minimal latency.
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
 *       // Use frame.buffer
 *       copyToOutputBuffer(frame.buffer, outputBuffer);
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
     */
    AcquiredFrame acquireLatestFrame(int timeoutMs = 0) {
        AcquiredFrame result;
        if (!mRing) return result;
        
        ControlRing::Slot slot;
        ControlRing::Slot latestSlot;
        bool hasSlot = false;
        
        // Drain ring, keeping only latest
        while (mRing->tryRead(&slot)) {
            // If we had a previous slot, release its buffer
            if (hasSlot) {
                mPool->releaseBuffer(latestSlot.bufferIndex);
                if (latestSlot.fenceFd >= 0) {
                    close(latestSlot.fenceFd);
                }
            }
            latestSlot = slot;
            hasSlot = true;
        }
        
        // If no frame yet, wait
        if (!hasSlot && timeoutMs > 0) {
            // Spin-wait with backoff (could use futex for better efficiency)
            int waited = 0;
            while (waited < timeoutMs) {
                if (mRing->tryRead(&latestSlot)) {
                    hasSlot = true;
                    break;
                }
                usleep(100);  // 0.1ms
                waited += 1;   // Approximate
            }
        }
        
        if (!hasSlot) return result;
        
        // Wait for GPU fence
        if (latestSlot.fenceFd >= 0) {
            sync_wait(latestSlot.fenceFd, 1000);  // 1 second timeout
            close(latestSlot.fenceFd);
        }
        
        result.bufferIndex = latestSlot.bufferIndex;
        result.buffer = mPool->getBuffer(latestSlot.bufferIndex);
        result.timestampNs = latestSlot.timestampNs;
        
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
        
        // Try to read
        if (!mRing->tryRead(&slot)) {
            if (timeoutMs <= 0) return result;
            
            // Wait for frame
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
        
        return result;
    }
    
    /**
     * Release frame back to renderer.
     */
    void releaseFrame(const AcquiredFrame& frame) {
        if (frame.valid()) {
            mPool->releaseBuffer(frame.bufferIndex);
            // TODO: Notify renderer that buffer is free
            // Could use another ring, eventfd, or Binder callback
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
