/*
 * AvailabilityBridge - pushes producer availability from VirtualCameraService
 * into the camera provider.
 *
 * The bridge owns a small thread that (re)connects to the
 * "virtual_camera_manager" binder service, registers an
 * IVirtualCameraHalListener, and invokes the supplied callback whenever
 * producer availability changes (including the initial state). The provider
 * uses this to dynamically add/remove the virtual camera device via
 * ICameraProviderCallback.cameraDeviceStatusChange(), so Camera2 availability
 * events track "a producer app is registered".
 *
 * Handles system_server not being up yet (retry loop) and system_server
 * restarts (binder death -> availability=false, then reconnect).
 *
 * Only built when VCAM_AIDL_SOURCE is defined (system_ext platform build).
 */
#pragma once

#ifdef VCAM_AIDL_SOURCE

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include <utils/StrongPointer.h>
#include <binder/IBinder.h>

namespace android::hardware::virtualcamera {
class IVirtualCameraManager;
class IVirtualCameraHalListener;
}

namespace virtualcamera {

class AvailabilityBridge {
public:
    using Callback = std::function<void(bool /*available*/)>;

    explicit AvailabilityBridge(Callback cb);
    ~AvailabilityBridge();

    void start();
    void stop();

    // Internal: called by the listener stub / death recipient.
    void onAvailability(bool available);
    void onManagerDied();

private:
    void threadLoop();
    bool connectLocked();

    Callback mCallback;
    std::mutex mLock;
    std::condition_variable mCv;
    std::thread mThread;
    bool mStop = false;
    bool mConnected = false;

    ::android::sp<::android::hardware::virtualcamera::IVirtualCameraManager> mManager;
    ::android::sp<::android::hardware::virtualcamera::IVirtualCameraHalListener> mListener;
    ::android::sp<::android::IBinder::DeathRecipient> mDeathRecipient;
};

}  // namespace virtualcamera

#endif  // VCAM_AIDL_SOURCE
