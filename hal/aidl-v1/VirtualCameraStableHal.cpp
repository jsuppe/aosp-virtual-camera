/*
 * VirtualCameraStableHal implementation (vendor/APEX build).
 */

#ifdef VCAM_STABLE_AIDL

#define LOG_TAG "VCamStableHal"

#include "VirtualCameraStableHal.h"
#include "VirtualCameraProvider.h"

#include <aidlcommonsupport/NativeHandle.h>
#include <android/binder_manager.h>
#include <vndk/hardware_buffer.h>
#include <log/log.h>

namespace aidl::android::hardware::camera::provider::implementation {

namespace {
std::shared_ptr<VirtualCameraStableHal> gInstance;
constexpr const char* kServiceName =
        "android.hardware.virtualcamera.hal.IVirtualCameraHal/default";
}  // namespace

bool VirtualCameraStableHal::init(VirtualCameraProvider* provider) {
    if (gInstance) return true;
    gInstance = ndk::SharedRefBase::make<VirtualCameraStableHal>();
    gInstance->mProvider = provider;
    binder_status_t st = AServiceManager_addService(
            gInstance->asBinder().get(), kServiceName);
    if (st != STATUS_OK) {
        ALOGE("Failed to register %s (status %d)", kServiceName, st);
        gInstance.reset();
        return false;
    }
    ALOGI("Registered %s", kServiceName);
    return true;
}

VirtualCameraStableHal* VirtualCameraStableHal::get() {
    return gInstance.get();
}

ndk::ScopedAStatus VirtualCameraStableHal::setCallback(
        const std::shared_ptr<HalCallback>& callback) {
    std::lock_guard<std::mutex> lock(mLock);
    mCallback = callback;
    ALOGI("Platform callback %s", callback ? "registered" : "cleared");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraStableHal::setProducerAvailable(bool available) {
    ALOGI("setProducerAvailable(%s)", available ? "true" : "false");
    if (mProvider) {
        mProvider->setProducerPresent(available);
    }
    if (!available) {
        // Drop the stale frame so a future session starts clean.
        std::lock_guard<std::mutex> lock(mLock);
        if (mLatest) {
            AHardwareBuffer_release(mLatest);
            mLatest = nullptr;
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraStableHal::queueFrame(
        const ::aidl::android::hardware::common::NativeHandle& buffer,
        int32_t width, int32_t height, int32_t stride, int32_t format,
        int64_t usage, int64_t timestampNs) {

    native_handle_t* handle = ::android::dupFromAidl(buffer);
    if (handle == nullptr) {
        ALOGE("queueFrame: bad handle");
        return ndk::ScopedAStatus::fromServiceSpecificError(1);
    }

    AHardwareBuffer_Desc desc = {};
    desc.width = static_cast<uint32_t>(width);
    desc.height = static_cast<uint32_t>(height);
    desc.layers = 1;
    desc.format = static_cast<uint32_t>(format);
    desc.usage = static_cast<uint64_t>(usage);
    desc.stride = static_cast<uint32_t>(stride);

    AHardwareBuffer* ahb = nullptr;
    int rc = AHardwareBuffer_createFromHandle(
            &desc, handle,
            AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE, &ahb);
    // CLONE copies the handle; release our dup either way.
    native_handle_close(handle);
    native_handle_delete(handle);

    if (rc != 0 || ahb == nullptr) {
        ALOGE("queueFrame: createFromHandle failed (%d)", rc);
        return ndk::ScopedAStatus::fromServiceSpecificError(2);
    }

    AHardwareBuffer* old;
    {
        std::lock_guard<std::mutex> lock(mLock);
        old = mLatest;
        mLatest = ahb;
        mLatestTs = timestampNs;
        mFramesReceived++;
        if (mFramesReceived % 150 == 0) {
            ALOGI("queueFrame: %llu frames received (%dx%d)",
                  (unsigned long long)mFramesReceived, width, height);
        }
    }
    if (old) AHardwareBuffer_release(old);
    return ndk::ScopedAStatus::ok();
}

AHardwareBuffer* VirtualCameraStableHal::acquireLatest(int64_t* timestampNs) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mLatest == nullptr) return nullptr;
    AHardwareBuffer_acquire(mLatest);
    if (timestampNs) *timestampNs = mLatestTs;
    return mLatest;
}

bool VirtualCameraStableHal::hasFrame() {
    std::lock_guard<std::mutex> lock(mLock);
    return mLatest != nullptr;
}

void VirtualCameraStableHal::notifyStreamsConfigured(int width, int height, int fps) {
    std::shared_ptr<HalCallback> cb;
    {
        std::lock_guard<std::mutex> lock(mLock);
        cb = mCallback;
    }
    if (cb) {
        ALOGI("notifyStreamsConfigured %dx%d@%d -> platform", width, height, fps);
        cb->onStreamsConfigured(width, height, fps);
    } else {
        ALOGW("Streams configured but no platform callback registered yet");
    }
}

void VirtualCameraStableHal::notifyCameraClosed() {
    std::shared_ptr<HalCallback> cb;
    {
        std::lock_guard<std::mutex> lock(mLock);
        cb = mCallback;
        if (mLatest) {
            AHardwareBuffer_release(mLatest);
            mLatest = nullptr;
        }
    }
    if (cb) cb->onCameraClosed();
}

}  // namespace aidl::android::hardware::camera::provider::implementation

#endif  // VCAM_STABLE_AIDL
