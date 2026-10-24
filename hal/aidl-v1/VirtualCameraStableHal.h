/*
 * VirtualCameraStableHal - vendor-side endpoint of the stable (VINTF) AIDL
 * boundary android.hardware.virtualcamera.hal.
 *
 * Hosted inside the virtual camera HAL process (vendor APEX). The platform
 * VirtualCameraService calls DOWN into this service (normal Treble
 * direction):
 *   - setProducerAvailable(bool)   -> dynamic camera add/remove
 *   - queueFrame(handle, ...)      -> V1 zero-copy frame delivery (unfenced)
 *   - queueFrameFenced(...)        -> V2: same, with the producer's acquire
 *                                     fence in and our read fence back out
 * and registers an IVirtualCameraHalCallback through which we notify stream
 * lifecycle (configureStreams / close) so the platform can create the
 * BufferQueue and relay its Surface to the producer app.
 *
 * Frame lifetime: we keep exactly one frame (the newest). The camera session
 * borrows it per capture request via acquireLatest()/releaseFrame(); each
 * release contributes the GPU read fence of that use. When the next frame
 * replaces it, the merged read fence is handed back to the platform as the
 * buffer's release fence.
 *
 * Only built when VCAM_STABLE_AIDL is defined (vendor/APEX build).
 */
#pragma once

#ifdef VCAM_STABLE_AIDL

#include <aidl/android/hardware/virtualcamera/hal/BnVirtualCameraHal.h>
#include <aidl/android/hardware/virtualcamera/hal/IVirtualCameraHalCallback.h>
#include <android/hardware_buffer.h>

#include "VirtualCameraSlots.h"
#include "ReadFenceSet.h"
#include "ProducerCapabilities.h"

#include <array>

#include <condition_variable>
#include <memory>
#include <mutex>

namespace aidl::android::hardware::camera::provider::implementation {

class VirtualCameraProvider;

class VirtualCameraStableHal
    : public ::aidl::android::hardware::virtualcamera::hal::BnVirtualCameraHal {
public:
    using HalCallback = ::aidl::android::hardware::virtualcamera::hal::IVirtualCameraHalCallback;
    using NativeHandle = ::aidl::android::hardware::common::NativeHandle;

    /** Create the singleton, bind it to the provider, register with servicemanager. */
    static bool init(VirtualCameraProvider* provider);
    static VirtualCameraStableHal* get();

    // ---- IVirtualCameraHal ----
    ndk::ScopedAStatus setCallback(
            const std::shared_ptr<HalCallback>& callback) override;
    ndk::ScopedAStatus setProducerAvailable(bool available) override;
    ndk::ScopedAStatus queueFrame(
            const NativeHandle& buffer,
            int32_t width, int32_t height, int32_t stride, int32_t format,
            int64_t usage, int64_t timestampNs) override;
    ndk::ScopedAStatus queueFrameFenced(
            const NativeHandle& buffer,
            int32_t width, int32_t height, int32_t stride, int32_t format,
            int64_t usage, int64_t timestampNs,
            const ndk::ScopedFileDescriptor& acquireFence,   // @nullable: fd -1 = null
            ndk::ScopedFileDescriptor* releaseFence) override;
    // V3
    ndk::ScopedAStatus getMaxCameras(int32_t* count) override;
    ndk::ScopedAStatus setCameraPresent(int32_t slot, bool present) override;
    ndk::ScopedAStatus queueFrameForCamera(
            int32_t slot, const NativeHandle& buffer,
            int32_t width, int32_t height, int32_t stride, int32_t format,
            int64_t usage, int64_t timestampNs,
            const ndk::ScopedFileDescriptor& acquireFence,
            ndk::ScopedFileDescriptor* releaseFence) override;
    ndk::ScopedAStatus queueFrameForConfiguration(
            int32_t slot, int64_t configurationToken, const NativeHandle& buffer,
            int32_t width, int32_t height, int32_t stride, int32_t format,
            int64_t usage, int64_t timestampNs, const ndk::ScopedFileDescriptor& acquireFence,
            ndk::ScopedFileDescriptor* releaseFence) override;

    // ---- hooks for the camera session (per slot) ----
    ndk::ScopedAStatus registerProducer(int32_t slot,
        const ::aidl::android::hardware::virtualcamera::hal::ProducerDescriptor& descriptor,
        const std::shared_ptr<HalCallback>& owner) override;
    ndk::ScopedAStatus removeProducer(int32_t slot, int64_t registration,
        const std::shared_ptr<HalCallback>& owner) override;
    ndk::ScopedAStatus setConfigurationReady(int32_t slot, int64_t token, bool ready) override;
    std::shared_ptr<const ::virtualcamera::ProducerCapabilities> capabilities(int slot);
    bool registrationCurrent(int slot, int64_t registration);
    /**
     * Newest producer frame for `slot`, +1 ref, marked in-flight.
     * *acquireFenceFd receives a dup of the producer's fence (-1 if none);
     * the caller must wait on it before reading and close it. Pair with
     * releaseFrame(). nullptr if no frame.
     */
    AHardwareBuffer* acquireLatest(int slot, int64_t* timestampNs, int* acquireFenceFd,
                                  uint64_t* sequence = nullptr, uint64_t afterSequence = 0,
                                  int64_t configurationToken = 0);
    /**
     * Done with a frame from acquireLatest(). readFenceFd (ownership passes,
     * -1 if the read already completed on the CPU) signals when this use's
     * GPU read is finished; it is merged into the frame's release fence.
     */
    void releaseFrame(int slot, AHardwareBuffer* ahb, int readFenceFd);
    // One capture can read the same source into several outputs. Keep its
    // reader reservation until ALL output dependencies have been recorded.
    void releaseFrame(int slot, AHardwareBuffer* ahb, std::vector<int> readFences);
    bool hasFrame(int slot);
    bool isFrameCurrent(int slot, AHardwareBuffer* buffer, uint64_t sequence,
                        int64_t configurationToken = 0);
    int64_t allocateSessionIdentity();
    int64_t notifyStreamsConfigured(int slot, int64_t owner, int width, int height, int fps);
    void notifyCameraClosed(int slot, int64_t owner, int64_t configurationToken);

    VirtualCameraStableHal() = default;

private:
    struct Frame {
        AHardwareBuffer* ahb = nullptr;  // owns one ref
        int64_t timestampNs = 0;
        uint64_t sequence = 0;           // submission identity, NOT timestamp/buffer identity
        int64_t configurationToken = 0;
        int acquireFence = -1;           // owned; producer's GPU-done fence
        std::vector<int> readFences;     // owned; all unmerged HAL dependencies
        int inflight = 0;                // acquireLatest() users not yet released
        bool retiring = false;           // closed/removed: no new readers
    };

    /** Shared body of the queueFrame* methods. Takes ownership of acquireFenceFd.
     *  Returns the retired frame's read fence (owned by caller, -1 if none). */
    ndk::ScopedAStatus enqueue(int slot, const NativeHandle& buffer, int32_t width,
                               int32_t height, int32_t stride, int32_t format,
                               int64_t usage, int64_t timestampNs,
                               int acquireFenceFd, int* retiredReadFence,
                               int64_t configurationToken = 0);
    /** Disable readers; retain frame until late readers and GPU work complete. */
    void dropLatestLocked(int slot);
    bool reapRetiringLocked(int slot);

    std::mutex mLock;
    std::condition_variable mCv;
    VirtualCameraProvider* mProvider = nullptr;
    std::shared_ptr<HalCallback> mCallback;
    int32_t mCallbackVersion = 0;   // platform callback's interface version
    std::array<Frame, ::virtualcamera::kMaxVirtualCameras> mLatest{};
    uint64_t mFramesReceived = 0;
    int64_t mNextIdentity = 0;
    std::array<int64_t, ::virtualcamera::kMaxVirtualCameras> mSessionOwners{}, mConfigurations{};
    std::array<bool, ::virtualcamera::kMaxVirtualCameras> mOwnerClosed{};
    std::array<std::shared_ptr<const ::virtualcamera::ProducerCapabilities>, ::virtualcamera::kMaxVirtualCameras> mCapabilities{};
    std::array<bool, ::virtualcamera::kMaxVirtualCameras> mReady{};
    std::array<int, ::virtualcamera::kMaxVirtualCameras> mSourceWidths{}, mSourceHeights{};
    std::mutex mPresenceLock; // serializes descriptor/publication/remove; never held by frame RPCs
};

}  // namespace aidl::android::hardware::camera::provider::implementation

#endif  // VCAM_STABLE_AIDL
