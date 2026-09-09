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
    int32_t ver = 0;
    if (callback && !callback->getInterfaceVersion(&ver).isOk()) ver = 1;
    std::lock_guard<std::mutex> lock(mLock);
    mCallback = callback;
    mCallbackVersion = ver;
    ALOGI("Platform callback %s (V%d: %s)", callback ? "registered" : "cleared", ver,
          ver >= 3 ? "per-slot callbacks" : "slot 0 only");
    return ndk::ScopedAStatus::ok();
}

int VirtualCameraStableHal::dropLatestLocked(int slot) {
    Frame f = mLatest[slot];
    mLatest[slot] = Frame{};
    if (f.ahb) AHardwareBuffer_release(f.ahb);  // in-flight readers hold their own ref
    closeFd(f.acquireFence);
    return f.readFence;
}

ndk::ScopedAStatus VirtualCameraStableHal::setProducerAvailable(bool available) {
    return setCameraPresent(0, available);   // V1/V2 == slot 0
}

ndk::ScopedAStatus VirtualCameraStableHal::getMaxCameras(int32_t* count) {
    *count = ::virtualcamera::kMaxVirtualCameras;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraStableHal::setCameraPresent(int32_t slot, bool present) {
    if (!::virtualcamera::validSlot(slot)) {
        return ndk::ScopedAStatus::fromServiceSpecificError(3);
    }
    ALOGI("setCameraPresent(slot %d, %s)", slot, present ? "true" : "false");
    if (mProvider) {
        mProvider->setSlotPresent(slot, present);
    }
    if (!present) {
        // Drop the stale frame so a future session starts clean.
        int fd;
        {
            std::lock_guard<std::mutex> lock(mLock);
            fd = dropLatestLocked(slot);
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
    auto st = enqueue(0, buffer, width, height, stride, format, usage,
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
    auto st = enqueue(0, buffer, width, height, stride, format, usage,
                      timestampNs, acq, &retired);
    releaseFence->set(retired);   // ownership -> parcel; -1 parcels as null
    return st;
}

ndk::ScopedAStatus VirtualCameraStableHal::queueFrameForCamera(
        int32_t slot, const NativeHandle& buffer,
        int32_t width, int32_t height, int32_t stride, int32_t format,
        int64_t usage, int64_t timestampNs,
        const ndk::ScopedFileDescriptor& acquireFence,
        ndk::ScopedFileDescriptor* releaseFence) {
    releaseFence->set(-1);
    if (!::virtualcamera::validSlot(slot)) {
        return ndk::ScopedAStatus::fromServiceSpecificError(3);
    }
    int acq = -1;
    if (acquireFence.get() >= 0) acq = ::dup(acquireFence.get());
    int retired = -1;
    auto st = enqueue(slot, buffer, width, height, stride, format, usage,
                      timestampNs, acq, &retired);
    releaseFence->set(retired);
    return st;
}

ndk::ScopedAStatus VirtualCameraStableHal::enqueue(
        int slot, const NativeHandle& buffer, int32_t width, int32_t height,
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
        Frame& cur = mLatest[slot];
        if (cur.ahb && cur.inflight > 0) {
            // A capture request is still reading the frame we are about to
            // retire; wait for it to submit and record its read fence.
            if (!mCv.wait_for(lock, kRetireWait,
                              [&cur] { return cur.inflight == 0; })) {
                ALOGW("queueFrame: retiring a frame with %d read(s) still in flight",
                      cur.inflight);
            }
        }
        old = cur;
        cur = Frame{};
        cur.ahb = ahb;
        cur.timestampNs = timestampNs;
        cur.acquireFence = acquireFenceFd;
        mFramesReceived++;
        if (mFramesReceived % 150 == 0) {
            ALOGI("queueFrame: %llu frames received (%dx%d, slot %d)%s",
                  (unsigned long long)mFramesReceived, width, height, slot,
                  acquireFenceFd >= 0 ? " [fenced]" : "");
        }
    }
    if (old.ahb) AHardwareBuffer_release(old.ahb);
    closeFd(old.acquireFence);
    *retiredReadFence = old.readFence;
    return ndk::ScopedAStatus::ok();
}

AHardwareBuffer* VirtualCameraStableHal::acquireLatest(int slot, int64_t* timestampNs,
                                                       int* acquireFenceFd) {
    std::lock_guard<std::mutex> lock(mLock);
    if (acquireFenceFd) *acquireFenceFd = -1;
    if (!::virtualcamera::validSlot(slot)) return nullptr;
    Frame& cur = mLatest[slot];
    if (cur.ahb == nullptr) return nullptr;
    AHardwareBuffer_acquire(cur.ahb);
    cur.inflight++;
    if (timestampNs) *timestampNs = cur.timestampNs;
    if (acquireFenceFd && cur.acquireFence >= 0) {
        *acquireFenceFd = ::dup(cur.acquireFence);
    }
    return cur.ahb;
}

void VirtualCameraStableHal::releaseFrame(int slot, AHardwareBuffer* ahb, int readFenceFd) {
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (::virtualcamera::validSlot(slot) && mLatest[slot].ahb == ahb) {
            Frame& cur = mLatest[slot];
            mergeFence(cur.readFence, readFenceFd);
            readFenceFd = -1;
            if (cur.inflight > 0) cur.inflight--;
            mCv.notify_all();
        }
        // else: the frame was already retired (timeout path) — nothing to
        // attach the fence to; the platform released it unfenced.
    }
    closeFd(readFenceFd);
    if (ahb) AHardwareBuffer_release(ahb);
}

bool VirtualCameraStableHal::hasFrame(int slot) {
    std::lock_guard<std::mutex> lock(mLock);
    return ::virtualcamera::validSlot(slot) && mLatest[slot].ahb != nullptr;
}

void VirtualCameraStableHal::notifyStreamsConfigured(int slot, int width, int height, int fps) {
    std::shared_ptr<HalCallback> cb;
    int32_t ver;
    {
        std::lock_guard<std::mutex> lock(mLock);
        cb = mCallback;
        ver = mCallbackVersion;
    }
    if (!cb) {
        ALOGW("Streams configured (slot %d) but no platform callback registered yet", slot);
        return;
    }
    ALOGI("notifyStreamsConfigured slot %d %dx%d@%d -> platform (V%d)", slot, width, height, fps, ver);
    if (ver >= 3) {
        cb->onStreamsConfiguredForCamera(slot, width, height, fps);
    } else if (slot == 0) {
        cb->onStreamsConfigured(width, height, fps);
    } else {
        ALOGW("slot %d configured but the platform callback is V%d (slot 0 only)", slot, ver);
    }
}

void VirtualCameraStableHal::notifyCameraClosed(int slot) {
    std::shared_ptr<HalCallback> cb;
    int32_t ver;
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(mLock);
        cb = mCallback;
        ver = mCallbackVersion;
        if (::virtualcamera::validSlot(slot)) fd = dropLatestLocked(slot);
    }
    closeFd(fd);
    if (!cb) return;
    if (ver >= 3) cb->onCameraClosedForCamera(slot);
    else if (slot == 0) cb->onCameraClosed();
}

}  // namespace aidl::android::hardware::camera::provider::implementation

#endif  // VCAM_STABLE_AIDL
