/*
 * VirtualCameraStableHal - vendor-side endpoint of the stable (VINTF) AIDL
 * boundary android.hardware.camera.virtual.
 *
 * Hosted inside the virtual camera HAL process (vendor APEX). The platform
 * VirtualCameraService calls DOWN into this service (normal Treble
 * direction):
 *   - setProducerAvailable(bool)  -> dynamic camera add/remove
 *   - queueFrame(handle, ...)     -> zero-copy frame delivery; we clone the
 *                                    gralloc handle into an AHardwareBuffer
 *                                    and keep only the newest frame
 * and registers an IVirtualCameraHalCallback through which we notify stream
 * lifecycle (configureStreams / close) so the platform can create the
 * BufferQueue and relay its Surface to the producer app.
 *
 * Only built when VCAM_STABLE_AIDL is defined (vendor/APEX build).
 */
#pragma once

#ifdef VCAM_STABLE_AIDL

#include <aidl/android/hardware/virtualcamera/hal/BnVirtualCameraHal.h>
#include <aidl/android/hardware/virtualcamera/hal/IVirtualCameraHalCallback.h>
#include <android/hardware_buffer.h>

#include <memory>
#include <mutex>

namespace aidl::android::hardware::camera::provider::implementation {

class VirtualCameraProvider;

class VirtualCameraStableHal
    : public ::aidl::android::hardware::virtualcamera::hal::BnVirtualCameraHal {
public:
    using HalCallback = ::aidl::android::hardware::virtualcamera::hal::IVirtualCameraHalCallback;

    /** Create the singleton, bind it to the provider, register with servicemanager. */
    static bool init(VirtualCameraProvider* provider);
    static VirtualCameraStableHal* get();

    // ---- IVirtualCameraHal ----
    ndk::ScopedAStatus setCallback(
            const std::shared_ptr<HalCallback>& callback) override;
    ndk::ScopedAStatus setProducerAvailable(bool available) override;
    ndk::ScopedAStatus queueFrame(
            const ::aidl::android::hardware::common::NativeHandle& buffer,
            int32_t width, int32_t height, int32_t stride, int32_t format,
            int64_t usage, int64_t timestampNs) override;

    // ---- hooks for the camera session ----
    /** Newest producer frame, +1 ref (caller releases). nullptr if none. */
    AHardwareBuffer* acquireLatest(int64_t* timestampNs);
    bool hasFrame();
    void notifyStreamsConfigured(int width, int height, int fps);
    void notifyCameraClosed();

    VirtualCameraStableHal() = default;

private:
    std::mutex mLock;
    VirtualCameraProvider* mProvider = nullptr;
    std::shared_ptr<HalCallback> mCallback;
    AHardwareBuffer* mLatest = nullptr;   // owns one ref
    int64_t mLatestTs = 0;
    uint64_t mFramesReceived = 0;
};

}  // namespace aidl::android::hardware::camera::provider::implementation

#endif  // VCAM_STABLE_AIDL
