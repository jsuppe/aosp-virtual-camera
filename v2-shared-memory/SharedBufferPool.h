/*
 * SharedBufferPool - Pre-allocated buffer pool shared between processes
 *
 * Buffers are allocated once at setup time and shared via file descriptors.
 * No allocation in the hot path = no latency spikes.
 *
 * Cross-process sharing uses AHardwareBuffer_sendHandleToUnixSocket /
 * AHardwareBuffer_recvHandleFromUnixSocket for the buffers, and ashmem
 * for the control ring.
 */
#pragma once

#include <android/hardware_buffer.h>
#ifdef __ANDROID__
  #include <android/sharedmem.h>
#else
  #include <cutils/ashmem.h>
#endif
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

namespace virtualcamera {

constexpr int MAX_BUFFERS = 4;
constexpr int RING_SIZE = 16;

/**
 * Control ring for lock-free producer/consumer communication.
 * Lives in shared memory (ashmem).
 *
 * The release ring allows the HAL to notify the renderer that a buffer
 * is free for reuse, completing the round-trip without Binder.
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

    // --- Release ring (HAL → Renderer) ---
    // Separate cache lines to avoid false sharing with the submit ring
    alignas(64) std::atomic<uint32_t> releaseWriteIndex{0};
    alignas(64) std::atomic<uint32_t> releaseReadIndex{0};

    struct ReleaseSlot {
        uint32_t bufferIndex;
    };

    ReleaseSlot releaseSlots[RING_SIZE];

    // Buffer ownership tracking (atomic per-buffer, shared across processes)
    // 0 = free (renderer may use), 1 = in-flight or HAL-owned
    alignas(64) std::atomic<uint8_t> bufferState[MAX_BUFFERS];

    // ----- Format negotiation (HAL → Renderer) -----
    // HAL writes the consumer's desired format after configureStreams().
    // Renderer polls negotiationSeq on each beginFrame(); when it changes,
    // the renderer should tear down its current pool, reallocate buffers
    // in the requested format, and reconnect.
    struct NegotiationBlock {
        std::atomic<uint32_t> seq{0};           // HAL increments on change
        std::atomic<int32_t>  format{0};        // AHARDWAREBUFFER_FORMAT_*
        std::atomic<int32_t>  width{0};
        std::atomic<int32_t>  height{0};
        std::atomic<uint64_t> usage{0};         // AHARDWAREBUFFER_USAGE_*
    };
    alignas(64) NegotiationBlock negotiation;

    // ----- Submit ring (Renderer → HAL) -----

    bool tryWrite(uint32_t bufferIdx, int fenceFd, int64_t timestamp) {
        uint32_t write = writeIndex.load(std::memory_order_relaxed);
        uint32_t read = readIndex.load(std::memory_order_acquire);

        if (write - read >= RING_SIZE) {
            return false;  // Full, drop frame
        }

        uint32_t slotIdx = write % RING_SIZE;
        slots[slotIdx].bufferIndex = bufferIdx;
        slots[slotIdx].fenceFd = fenceFd;
        slots[slotIdx].timestampNs = timestamp;
        slots[slotIdx].flags = 0;

        writeIndex.store(write + 1, std::memory_order_release);
        return true;
    }

    bool tryRead(Slot* out) {
        uint32_t read = readIndex.load(std::memory_order_relaxed);
        uint32_t write = writeIndex.load(std::memory_order_acquire);

        if (read >= write) {
            return false;  // Empty
        }

        uint32_t slotIdx = read % RING_SIZE;
        *out = slots[slotIdx];

        readIndex.store(read + 1, std::memory_order_release);
        return true;
    }

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

    // ----- Release ring (HAL → Renderer) -----

    bool tryWriteRelease(uint32_t bufferIdx) {
        uint32_t write = releaseWriteIndex.load(std::memory_order_relaxed);
        uint32_t read = releaseReadIndex.load(std::memory_order_acquire);

        if (write - read >= RING_SIZE) {
            return false;
        }

        releaseSlots[write % RING_SIZE].bufferIndex = bufferIdx;
        // Also update the shared buffer state
        bufferState[bufferIdx].store(0, std::memory_order_release);

        releaseWriteIndex.store(write + 1, std::memory_order_release);
        return true;
    }

    bool tryReadRelease(ReleaseSlot* out) {
        uint32_t read = releaseReadIndex.load(std::memory_order_relaxed);
        uint32_t write = releaseWriteIndex.load(std::memory_order_acquire);

        if (read >= write) {
            return false;
        }

        *out = releaseSlots[read % RING_SIZE];

        releaseReadIndex.store(read + 1, std::memory_order_release);
        return true;
    }

    uint32_t releasesAvailable() const {
        uint32_t write = releaseWriteIndex.load(std::memory_order_acquire);
        uint32_t read = releaseReadIndex.load(std::memory_order_relaxed);
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
 *   // Send controlFd + AHardwareBuffers to process B via Unix socket
 *
 *   // Process B (attaches to existing pool):
 *   SharedBufferPool pool;
 *   pool.attachControlRing(controlFd);
 *   pool.recvBuffersFrom(socketFd, bufferCount, config);
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
        if (bufferCount > MAX_BUFFERS || bufferCount <= 0) return false;

        mConfig.width = width;
        mConfig.height = height;
        mConfig.format = format;
        mConfig.bufferCount = bufferCount;
        mConfig.usage = usage | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
                              | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;

        // Allocate control ring in shared memory
#ifdef __ANDROID__
        mControlRingFd = ASharedMemory_create("vcam_ring", sizeof(ControlRing));
#else
        mControlRingFd = ashmem_create_region("vcam_ring", sizeof(ControlRing));
#endif
        if (mControlRingFd < 0) return false;

        mControlRing = static_cast<ControlRing*>(
            mmap(nullptr, sizeof(ControlRing), PROT_READ | PROT_WRITE,
                 MAP_SHARED, mControlRingFd, 0));
        if (mControlRing == MAP_FAILED) {
            mControlRing = nullptr;
            close(mControlRingFd);
            mControlRingFd = -1;
            return false;
        }

        // Placement-new to initialize atomics
        new (mControlRing) ControlRing();

        // Mark all buffers as free
        for (int i = 0; i < bufferCount; i++) {
            mControlRing->bufferState[i].store(0, std::memory_order_relaxed);
        }

        // Allocate hardware buffers
        AHardwareBuffer_Desc desc = {};
        desc.width = static_cast<uint32_t>(width);
        desc.height = static_cast<uint32_t>(height);
        desc.layers = 1;
        desc.format = static_cast<uint32_t>(format);
        desc.usage = mConfig.usage;

        for (int i = 0; i < bufferCount; i++) {
            if (AHardwareBuffer_allocate(&desc, &mBuffers[i]) != 0) {
                release();
                return false;
            }
        }

        mBufferCount = bufferCount;
        mOwner = true;
        return true;
    }

    /**
     * Attach to an existing control ring fd (receiver side, step 1).
     * After this, call recvBuffersFrom() to receive the AHardwareBuffers.
     */
    bool attachControlRing(int controlRingFd) {
        mControlRingFd = dup(controlRingFd);
        if (mControlRingFd < 0) return false;

        mControlRing = static_cast<ControlRing*>(
            mmap(nullptr, sizeof(ControlRing), PROT_READ | PROT_WRITE,
                 MAP_SHARED, mControlRingFd, 0));
        if (mControlRing == MAP_FAILED) {
            mControlRing = nullptr;
            close(mControlRingFd);
            mControlRingFd = -1;
            return false;
        }

        mOwner = false;
        return true;
    }

    /**
     * Receive AHardwareBuffers over a Unix socket (receiver side, step 2).
     * The sender must call sendBuffersTo() on their side.
     */
    bool recvBuffersFrom(int socketFd, int bufferCount,
                         const BufferPoolConfig& config) {
        if (bufferCount > MAX_BUFFERS || bufferCount <= 0) return false;
        if (!mControlRing) return false;

        mConfig = config;

        for (int i = 0; i < bufferCount; i++) {
            int ret = AHardwareBuffer_recvHandleFromUnixSocket(
                socketFd, &mBuffers[i]);
            if (ret != 0) {
                // Clean up any already-received buffers
                for (int j = 0; j < i; j++) {
                    AHardwareBuffer_release(mBuffers[j]);
                    mBuffers[j] = nullptr;
                }
                return false;
            }
        }

        mBufferCount = bufferCount;
        return true;
    }

    /**
     * Send AHardwareBuffers over a Unix socket (creator side).
     * The receiver must call recvBuffersFrom() on their side.
     */
    bool sendBuffersTo(int socketFd) const {
        for (int i = 0; i < mBufferCount; i++) {
            int ret = AHardwareBuffer_sendHandleToUnixSocket(
                mBuffers[i], socketFd);
            if (ret != 0) return false;
        }
        return true;
    }

    void release() {
        for (int i = 0; i < mBufferCount; i++) {
            if (mBuffers[i]) {
                AHardwareBuffer_release(mBuffers[i]);
                mBuffers[i] = nullptr;
            }
        }

        if (mControlRing) {
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
    AHardwareBuffer* getBuffer(int index) {
        if (index < 0 || index >= mBufferCount) return nullptr;
        return mBuffers[index];
    }
    int getBufferCount() const { return mBufferCount; }
    const BufferPoolConfig& getConfig() const { return mConfig; }

    /**
     * Acquire a free buffer for rendering (renderer side).
     * Uses the shared atomic bufferState in the control ring so that
     * both processes agree on ownership.
     * Returns buffer index, or -1 if none available.
     */
    int acquireFreeBuffer() {
        if (!mControlRing) return -1;
        for (int i = 0; i < mBufferCount; i++) {
            uint8_t expected = 0;
            if (mControlRing->bufferState[i].compare_exchange_strong(
                    expected, 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return i;
            }
        }
        return -1;
    }

    /**
     * Release a buffer back to the free pool.
     * Typically called by the HAL via the release ring, or by the renderer
     * if a frame was never submitted.
     */
    void releaseBuffer(int index) {
        if (index >= 0 && index < mBufferCount && mControlRing) {
            mControlRing->bufferState[index].store(0, std::memory_order_release);
        }
    }

    /**
     * Check if a buffer is currently free.
     */
    bool isBufferFree(int index) const {
        if (index < 0 || index >= mBufferCount || !mControlRing) return false;
        return mControlRing->bufferState[index].load(std::memory_order_acquire) == 0;
    }

private:
    BufferPoolConfig mConfig{};
    ControlRing* mControlRing = nullptr;
    int mControlRingFd = -1;

    AHardwareBuffer* mBuffers[MAX_BUFFERS] = {};
    int mBufferCount = 0;
    bool mOwner = false;
};

}  // namespace virtualcamera
