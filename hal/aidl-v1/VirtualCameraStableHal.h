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

    // ---- hooks for the camera session ----
    /**
     * Newest producer frame, +1 ref, marked in-flight. *acquireFenceFd
     * receives a dup of the producer's fence (-1 if none); the caller must
     * wait on it before reading and close it. Pair with releaseFrame().
     * nullptr if no frame.
     */
    AHardwareBuffer* acquireLatest(int64_t* timestampNs, int* acquireFenceFd);
    /**
     * Done with a frame from acquireLatest(). readFenceFd (ownership passes,
     * -1 if the read already completed on the CPU) signals when this use's
     * GPU read is finished; it is merged into the frame's release fence.
     */
    void releaseFrame(AHardwareBuffer* ahb, int readFenceFd);
    bool hasFrame();
    void notifyStreamsConfigured(int width, int height, int fps);
    void notifyCameraClosed();

    VirtualCameraStableHal() = default;

private:
    struct Frame {
        AHardwareBuffer* ahb = nullptr;  // owns one ref
        int64_t timestampNs = 0;
        int acquireFence = -1;           // owned; producer's GPU-done fence
        int readFence = -1;              // owned; merged HAL read-done fences
        int inflight = 0;                // acquireLatest() users not yet released
    };

    /** Shared body of queueFrame/queueFrameFenced. Takes ownership of acquireFenceFd.
     *  Returns the retired frame's read fence (owned by caller, -1 if none). */
    ndk::ScopedAStatus enqueue(const NativeHandle& buffer, int32_t width,
                               int32_t height, int32_t stride, int32_t format,
                               int64_t usage, int64_t timestampNs,
                               int acquireFenceFd, int* retiredReadFence);
    /** Drop the current frame (locked). Returns its read fence (owned). */
    int dropLatestLocked();

    std::mutex mLock;
    std::condition_variable mCv;
    VirtualCameraProvider* mProvider = nullptr;
    std::shared_ptr<HalCallback> mCallback;
    Frame mLatest;
    uint64_t mFramesReceived = 0;
};

}  // namespace aidl::android::hardware::camera::provider::implementation

#endif  // VCAM_STABLE_AIDL
