/*
 * SharedBufferPool - Pre-allocated buffer pool shared between processes
 * 
 * Buffers are allocated once at setup time and shared via file descriptors.
 * No allocation in the hot path = no latency spikes.
 */
#pragma once

#include <android/hardware_buffer.h>
#include <cutils/ashmem.h>
#include <sys/mman.h>
#include <atomic>
#include <cstdint>

namespace virtualcamera {

constexpr int MAX_BUFFERS = 4;
constexpr int RING_SIZE = 16;

/**
 * Control ring for lock-free producer/consumer communication.
 * Lives in shared memory (ashmem).
 */
struct ControlRing {
    // Cache-line aligned to prevent false sharing
    alignas(64) std::atomic<uint32_t> writeIndex{0};
    alignas(64) std::atomic<uint32_t> readIndex{0};
    
    struct Slot {
        uint32_t bufferIndex;      // Index into buffer pool
        int32_t fenceFd;           // Sync fence FD (-1 if none/signaled)
        int64_t timestampNs;       // Frame timestamp
        uint32_t flags;            // Reserved
    };
    
    Slot slots[RING_SIZE];
    
    // Producer (renderer) operations
    bool tryWrite(uint32_t bufferIdx, int fenceFd, int64_t timestamp) {
        uint32_t write = writeIndex.load(std::memory_order_relaxed);
        uint32_t read = readIndex.load(std::memory_order_acquire);
        
        // Check if ring is full
        if (write - read >= RING_SIZE) {
            return false;  // Full, drop frame
        }
        
        uint32_t slotIdx = write % RING_SIZE;
        slots[slotIdx].bufferIndex = bufferIdx;
        slots[slotIdx].fenceFd = fenceFd;
        slots[slotIdx].timestampNs = timestamp;
        slots[slotIdx].flags = 0;
        
        // Memory barrier before publishing
        writeIndex.store(write + 1, std::memory_order_release);
        return true;
    }
    
    // Consumer (HAL) operations
    bool tryRead(Slot* out) {
        uint32_t read = readIndex.load(std::memory_order_relaxed);
        uint32_t write = writeIndex.load(std::memory_order_acquire);
        
        // Check if ring is empty
        if (read >= write) {
            return false;  // Empty
        }
        
        uint32_t slotIdx = read % RING_SIZE;
        *out = slots[slotIdx];
        
        readIndex.store(read + 1, std::memory_order_release);
        return true;
    }
    
    // Peek without consuming
    bool peek(Slot* out) {
        uint32_t read = readIndex.load(std::memory_order_relaxed);
        uint32_t write = writeIndex.load(std::memory_order_acquire);
        
        if (read >= write) {
            return false;
        }
        
        *out = slots[read % RING_SIZE];
        return true;
    }
    
    uint32_t available() const {
        uint32_t write = writeIndex.load(std::memory_order_acquire);
        uint32_t read = readIndex.load(std::memory_order_relaxed);
        return write - read;
    }
};

/**
 * Shared buffer pool configuration.
 * Passed during setup, used to recreate pool in other process.
 */
struct BufferPoolConfig {
    int32_t width;
    int32_t height;
    int32_t format;         // AHARDWAREBUFFER_FORMAT_*
    int32_t bufferCount;
    uint64_t usage;         // AHARDWAREBUFFER_USAGE_*
};

/**
 * Buffer pool manager.
 * 
 * Usage:
 *   // Process A (creates pool):
 *   SharedBufferPool pool;
 *   pool.allocate(3840, 2160, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 4);
 *   int controlFd = pool.getControlRingFd();
 *   // Send controlFd + buffer FDs to process B via Binder
 *   
 *   // Process B (attaches to existing pool):
 *   SharedBufferPool pool;
 *   pool.attach(controlFd, bufferFds, config);
 */
class SharedBufferPool {
public:
    SharedBufferPool() = default;
    ~SharedBufferPool() { release(); }
    
    // Non-copyable
    SharedBufferPool(const SharedBufferPool&) = delete;
    SharedBufferPool& operator=(const SharedBufferPool&) = delete;
    
    /**
     * Allocate a new buffer pool (creator side).
     */
    bool allocate(int32_t width, int32_t height, int32_t format, 
                  int32_t bufferCount, uint64_t usage = 0) {
        if (bufferCount > MAX_BUFFERS) return false;
        
        mConfig.width = width;
        mConfig.height = height;
        mConfig.format = format;
        mConfig.bufferCount = bufferCount;
        mConfig.usage = usage | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE 
                              | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
        
        // Allocate control ring in shared memory
        mControlRingFd = ashmem_create_region("vcam_ring", sizeof(ControlRing));
        if (mControlRingFd < 0) return false;
        
        mControlRing = static_cast<ControlRing*>(
            mmap(nullptr, sizeof(ControlRing), PROT_READ | PROT_WRITE,
                 MAP_SHARED, mControlRingFd, 0));
        if (mControlRing == MAP_FAILED) {
            close(mControlRingFd);
            return false;
        }
        
        // Initialize ring
        new (mControlRing) ControlRing();
        
        // Allocate hardware buffers
        AHardwareBuffer_Desc desc = {
            .width = static_cast<uint32_t>(width),
            .height = static_cast<uint32_t>(height),
            .layers = 1,
            .format = static_cast<uint32_t>(format),
            .usage = mConfig.usage,
        };
        
        for (int i = 0; i < bufferCount; i++) {
            if (AHardwareBuffer_allocate(&desc, &mBuffers[i]) != 0) {
                release();
                return false;
            }
            mBufferFree[i] = true;
        }
        
        mBufferCount = bufferCount;
        mOwner = true;
        return true;
    }
    
    /**
     * Attach to existing pool (receiver side).
     */
    bool attach(int controlRingFd, const int* bufferFds, int bufferCount,
                const BufferPoolConfig& config) {
        mConfig = config;
        mControlRingFd = dup(controlRingFd);
        
        mControlRing = static_cast<ControlRing*>(
            mmap(nullptr, sizeof(ControlRing), PROT_READ | PROT_WRITE,
                 MAP_SHARED, mControlRingFd, 0));
        if (mControlRing == MAP_FAILED) {
            close(mControlRingFd);
            return false;
        }
        
        // Import hardware buffers from file descriptors
        for (int i = 0; i < bufferCount; i++) {
            // Create native_handle from fd
            native_handle_t* handle = native_handle_create(1, 0);
            handle->data[0] = dup(bufferFds[i]);
            
            AHardwareBuffer_Desc desc = {
                .width = static_cast<uint32_t>(config.width),
                .height = static_cast<uint32_t>(config.height),
                .layers = 1,
                .format = static_cast<uint32_t>(config.format),
                .usage = config.usage,
            };
            
            // This would use AHardwareBuffer_createFromHandle in real impl
            // For now, just store the fd
            mBufferFds[i] = handle->data[0];
            native_handle_delete(handle);
        }
        
        mBufferCount = bufferCount;
        mOwner = false;
        return true;
    }
    
    void release() {
        if (mOwner) {
            for (int i = 0; i < mBufferCount; i++) {
                if (mBuffers[i]) {
                    AHardwareBuffer_release(mBuffers[i]);
                    mBuffers[i] = nullptr;
                }
            }
        }
        
        if (mControlRing && mControlRing != MAP_FAILED) {
            munmap(mControlRing, sizeof(ControlRing));
            mControlRing = nullptr;
        }
        
        if (mControlRingFd >= 0) {
            close(mControlRingFd);
            mControlRingFd = -1;
        }
        
        mBufferCount = 0;
    }
    
    // Accessors
    ControlRing* getControlRing() { return mControlRing; }
    int getControlRingFd() const { return mControlRingFd; }
    AHardwareBuffer* getBuffer(int index) { return mBuffers[index]; }
    int getBufferCount() const { return mBufferCount; }
    const BufferPoolConfig& getConfig() const { return mConfig; }
    
    /**
     * Acquire a free buffer for rendering.
     * Returns buffer index, or -1 if none available.
     */
    int acquireFreeBuffer() {
        for (int i = 0; i < mBufferCount; i++) {
            if (mBufferFree[i]) {
                mBufferFree[i] = false;
                return i;
            }
        }
        return -1;
    }
    
    /**
     * Release a buffer back to the free pool.
     */
    void releaseBuffer(int index) {
        if (index >= 0 && index < mBufferCount) {
            mBufferFree[index] = true;
        }
    }
    
    /**
     * Get file descriptor for a buffer (for sharing).
     */
    int getBufferFd(int index) {
        if (!mBuffers[index]) return -1;
        
        int fd = -1;
        // AHardwareBuffer_getNativeHandle would give us the fd
        // Simplified: use internal fd storage
        return mBufferFds[index];
    }

private:
    BufferPoolConfig mConfig{};
    ControlRing* mControlRing = nullptr;
    int mControlRingFd = -1;
    
    AHardwareBuffer* mBuffers[MAX_BUFFERS] = {};
    int mBufferFds[MAX_BUFFERS] = {-1, -1, -1, -1};
    bool mBufferFree[MAX_BUFFERS] = {true, true, true, true};
    int mBufferCount = 0;
    bool mOwner = false;
};

}  // namespace virtualcamera
