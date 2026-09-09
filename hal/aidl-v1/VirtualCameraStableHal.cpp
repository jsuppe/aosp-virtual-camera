/*
 * VirtualCameraStableHal implementation (vendor/APEX build).
 */

#ifdef VCAM_STABLE_AIDL

#define LOG_TAG "VCamStableHal"

#include "VirtualCameraStableHal.h"
#include "VirtualCameraProvider.h"

#include <aidlcommonsupport/NativeHandle.h>
#include <android/binder_manager.h>
#include <android/sync.h>
#include <vndk/hardware_buffer.h>
#include <log/log.h>
#include <unistd.h>

#include <chrono>

namespace aidl::android::hardware::camera::provider::implementation {

namespace {
std::shared_ptr<VirtualCameraStableHal> gInstance;
constexpr const char* kServiceName =
        "android.hardware.virtualcamera.hal.IVirtualCameraHal/default";

// How long queueFrame waits for an in-flight read of the frame it retires
// to hand in its fence. A composite submits in well under a millisecond
// (it does not wait for the GPU), so this only trips on a wedged GPU.
constexpr auto kRetireWait = std::chrono::milliseconds(200);

void closeFd(int& fd) {
    if (fd >= 0) { ::close(fd); fd = -1; }
}

/** Merge b into a (both owned); result owned in a. */
void mergeFence(int& a, int b) {
    if (b < 0) return;
    if (a < 0) { a = b; return; }
    int m = sync_merge("vcam_read", a, b);
    ::close(b);
    if (m >= 0) { ::close(a); a = m; }
    // else: keep a; the newest read's fence is the likeliest to be last anyway
}
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
    ALOGI("Registered %s (interface V%d)", kServiceName,
          VirtualCameraStableHal::version);
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

int VirtualCameraStableHal::dropLatestLocked() {
    Frame f = mLatest;
    mLatest = Frame{};
    if (f.ahb) AHardwareBuffer_release(f.ahb);  // in-flight readers hold their own ref
    closeFd(f.acquireFence);
    return f.readFence;
}

ndk::ScopedAStatus VirtualCameraStableHal::setProducerAvailable(bool available) {
    ALOGI("setProducerAvailable(%s)", available ? "true" : "false");
    if (mProvider) {
        mProvider->setProducerPresent(available);
    }
    if (!available) {
        // Drop the stale frame so a future session starts clean.
        int fd;
        {
            std::lock_guard<std::mutex> lock(mLock);
            fd = dropLatestLocked();
        }
        closeFd(fd);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraStableHal::queueFrame(
        const NativeHandle& buffer,
        int32_t width, int32_t height, int32_t stride, int32_t format,
        int64_t usage, int64_t timestampNs) {
    // V1 caller: no fences either way. The retired frame's read fence is
    // dropped — the V1 platform releases buffers unfenced (its original
    // contract); V2 callers get it back.
    int retired = -1;
    auto st = enqueue(buffer, width, height, stride, format, usage,
                      timestampNs, /*acquireFenceFd*/ -1, &retired);
    closeFd(retired);
    return st;
}

ndk::ScopedAStatus VirtualCameraStableHal::queueFrameFenced(
        const NativeHandle& buffer,
        int32_t width, int32_t height, int32_t stride, int32_t format,
        int64_t usage, int64_t timestampNs,
        const ndk::ScopedFileDescriptor& acquireFence,
        ndk::ScopedFileDescriptor* releaseFence) {
    int acq = -1;
    if (acquireFence.get() >= 0) {
        acq = ::dup(acquireFence.get());   // the binder-owned fd dies with the call
    }
    int retired = -1;
    auto st = enqueue(buffer, width, height, stride, format, usage,
                      timestampNs, acq, &retired);
    releaseFence->set(retired);   // ownership -> parcel; -1 parcels as null
    return st;
}

ndk::ScopedAStatus VirtualCameraStableHal::enqueue(
        const NativeHandle& buffer, int32_t width, int32_t height,
        int32_t stride, int32_t format, int64_t usage, int64_t timestampNs,
        int acquireFenceFd, int* retiredReadFence) {
    *retiredReadFence = -1;

    native_handle_t* handle = ::android::dupFromAidl(buffer);
    if (handle == nullptr) {
        ALOGE("queueFrame: bad handle");
        closeFd(acquireFenceFd);
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
        closeFd(acquireFenceFd);
        return ndk::ScopedAStatus::fromServiceSpecificError(2);
    }

    Frame old;
    {
        std::unique_lock<std::mutex> lock(mLock);
        if (mLatest.ahb && mLatest.inflight > 0) {
            // A capture request is still reading the frame we are about to
            // retire; wait for it to submit and record its read fence.
            if (!mCv.wait_for(lock, kRetireWait,
                              [this] { return mLatest.inflight == 0; })) {
                ALOGW("queueFrame: retiring a frame with %d read(s) still in flight",
                      mLatest.inflight);
            }
        }
        old = mLatest;
        mLatest = Frame{};
        mLatest.ahb = ahb;
        mLatest.timestampNs = timestampNs;
        mLatest.acquireFence = acquireFenceFd;
        mFramesReceived++;
        if (mFramesReceived % 150 == 0) {
            ALOGI("queueFrame: %llu frames received (%dx%d)%s",
                  (unsigned long long)mFramesReceived, width, height,
                  acquireFenceFd >= 0 ? " [fenced]" : "");
        }
    }
    if (old.ahb) AHardwareBuffer_release(old.ahb);
    closeFd(old.acquireFence);
    *retiredReadFence = old.readFence;
    return ndk::ScopedAStatus::ok();
}

AHardwareBuffer* VirtualCameraStableHal::acquireLatest(int64_t* timestampNs,
                                                       int* acquireFenceFd) {
    std::lock_guard<std::mutex> lock(mLock);
    if (acquireFenceFd) *acquireFenceFd = -1;
    if (mLatest.ahb == nullptr) return nullptr;
    AHardwareBuffer_acquire(mLatest.ahb);
    mLatest.inflight++;
    if (timestampNs) *timestampNs = mLatest.timestampNs;
    if (acquireFenceFd && mLatest.acquireFence >= 0) {
        *acquireFenceFd = ::dup(mLatest.acquireFence);
    }
    return mLatest.ahb;
}

void VirtualCameraStableHal::releaseFrame(AHardwareBuffer* ahb, int readFenceFd) {
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (mLatest.ahb == ahb) {
            mergeFence(mLatest.readFence, readFenceFd);
            readFenceFd = -1;
            if (mLatest.inflight > 0) mLatest.inflight--;
            mCv.notify_all();
        }
        // else: the frame was already retired (timeout path) — nothing to
        // attach the fence to; the platform released it unfenced.
    }
    closeFd(readFenceFd);
    if (ahb) AHardwareBuffer_release(ahb);
}

bool VirtualCameraStableHal::hasFrame() {
    std::lock_guard<std::mutex> lock(mLock);
    return mLatest.ahb != nullptr;
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
    int fd;
    {
        std::lock_guard<std::mutex> lock(mLock);
        cb = mCallback;
        fd = dropLatestLocked();
    }
    closeFd(fd);
    if (cb) cb->onCameraClosed();
}

}  // namespace aidl::android::hardware::camera::provider::implementation

#endif  // VCAM_STABLE_AIDL
