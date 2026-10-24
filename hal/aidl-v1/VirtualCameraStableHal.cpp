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
#include <limits>
#include <utility>

namespace aidl::android::hardware::camera::provider::implementation {

namespace {
std::shared_ptr<VirtualCameraStableHal> gInstance;
constexpr const char* kServiceName =
        "android.hardware.virtualcamera.hal.IVirtualCameraHal/default";

// How long queueFrame waits for an in-flight read of the frame it retires
// to hand in its fence. Timeout rejects the incoming frame; it must never
// retire a frame whose readers have not yet submitted their completion fences.
constexpr auto kRetireWait = std::chrono::milliseconds(200);
constexpr size_t kMaxPendingReads = 16;

void closeFd(int& fd) {
    if (fd >= 0) { ::close(fd); fd = -1; }
}

bool consolidateFences(std::vector<int>& fences) {
    return ::virtualcamera::consolidateReadFences(fences,
            [](int fd, int ms) { return sync_wait(fd, ms); },
            [](int a, int b) { return sync_merge("vcam_read", a, b); },
            [](int fd) { ::close(fd); });
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
    if (callback && ver < 6) return ndk::ScopedAStatus::fromServiceSpecificError(7);
    std::lock_guard<std::mutex> publication(mPresenceLock);
    bool replaced = false;
    {
        std::lock_guard<std::mutex> lock(mLock);
        replaced = mCallback && mCallback->asBinder().get() != (callback ? callback->asBinder().get() : nullptr);
        if (replaced) {
            for (int slot=0;slot<::virtualcamera::kMaxVirtualCameras;slot++) {
                mConfigurations[slot]=0;mOwnerClosed[slot]=true;mReady[slot]=false;
                mCapabilities[slot].reset();dropLatestLocked(slot);
                if(mNextIdentity<std::numeric_limits<int64_t>::max())mSessionOwners[slot]=++mNextIdentity;
            }
        }
        mCallback=callback;mCallbackVersion=ver;
    }
    if(replaced && mProvider) for(int slot=0;slot<::virtualcamera::kMaxVirtualCameras;slot++)mProvider->setSlotPresent(slot,false);
    return ndk::ScopedAStatus::ok();
}

bool VirtualCameraStableHal::reapRetiringLocked(int slot) {
    Frame& cur = mLatest[slot];
    if (!cur.retiring) return true;
    if (!::virtualcamera::retiredFrameReady(cur.inflight, cur.readFences,
            [](int fd, int ms) { return sync_wait(fd, ms); },
            [](int fd) { ::close(fd); })) return false;
    if (cur.ahb) AHardwareBuffer_release(cur.ahb);
    closeFd(cur.acquireFence);
    cur = Frame{};
    return true;
}

void VirtualCameraStableHal::dropLatestLocked(int slot) {
    mLatest[slot].retiring = true;
    reapRetiringLocked(slot);  // poll only; preserve late readers/dependencies
}

std::shared_ptr<const ::virtualcamera::ProducerCapabilities> VirtualCameraStableHal::capabilities(int slot) {
    std::lock_guard<std::mutex> lock(mLock);
    return ::virtualcamera::validSlot(slot) ? mCapabilities[slot] : nullptr;
}
bool VirtualCameraStableHal::registrationCurrent(int slot,int64_t registration) {
    auto c=capabilities(slot);return c && c->registration==registration;
}
ndk::ScopedAStatus VirtualCameraStableHal::registerProducer(int32_t slot,
        const ::aidl::android::hardware::virtualcamera::hal::ProducerDescriptor& d,
        const std::shared_ptr<HalCallback>& owner) {
    auto c=std::make_shared<::virtualcamera::ProducerCapabilities>();
    c->registration=d.registration;c->facing=d.facing;c->orientation=d.orientation;c->maxStreams=d.maxOutputStreams;
    for(const auto& m:d.modes)c->modes.push_back({m.id,m.width,m.height,m.minFps,m.maxFps,m.outputFormats,m.allowDownscale});
    if(!::virtualcamera::validSlot(slot)||!c->valid()||!owner)return ndk::ScopedAStatus::fromServiceSpecificError(8);
    std::lock_guard<std::mutex> publication(mPresenceLock);
    {
        std::lock_guard<std::mutex> lock(mLock);
        if(!mCallback || mCallback->asBinder().get()!=owner->asBinder().get() || mCapabilities[slot])
            return ndk::ScopedAStatus::fromServiceSpecificError(8);
        mCapabilities[slot]=c;mReady[slot]=false;mConfigurations[slot]=0;
    }
    if(mProvider)mProvider->setSlotPresent(slot,true);
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus VirtualCameraStableHal::removeProducer(int32_t slot,int64_t registration,
        const std::shared_ptr<HalCallback>& owner) {
    if(!::virtualcamera::validSlot(slot)||!owner)return ndk::ScopedAStatus::fromServiceSpecificError(8);
    std::lock_guard<std::mutex> publication(mPresenceLock);
    {
        std::lock_guard<std::mutex> lock(mLock);
        if(!mCallback || mCallback->asBinder().get()!=owner->asBinder().get() ||
           !mCapabilities[slot] || mCapabilities[slot]->registration!=registration)
            return ndk::ScopedAStatus::fromServiceSpecificError(8);
        mReady[slot]=false;mConfigurations[slot]=0;mOwnerClosed[slot]=true;
        mCapabilities[slot].reset();dropLatestLocked(slot);
        if(mNextIdentity<std::numeric_limits<int64_t>::max())mSessionOwners[slot]=++mNextIdentity;
    }
    if(mProvider)mProvider->setSlotPresent(slot,false);
    return ndk::ScopedAStatus::ok();
}
ndk::ScopedAStatus VirtualCameraStableHal::setConfigurationReady(int32_t slot,int64_t token,bool ready) {
    std::lock_guard<std::mutex> lock(mLock);
    if(!::virtualcamera::validSlot(slot)||token<=0||mConfigurations[slot]!=token||!mCapabilities[slot]||mOwnerClosed[slot])
        return ndk::ScopedAStatus::fromServiceSpecificError(8);
    mReady[slot]=ready;
    if(!ready){mConfigurations[slot]=0;dropLatestLocked(slot);}
    mCv.notify_all();return ndk::ScopedAStatus::ok();
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
    if (mProvider) return ndk::ScopedAStatus::fromServiceSpecificError(8);
    ALOGI("setCameraPresent(slot %d, %s)", slot, present ? "true" : "false");
    if (!present) {
        std::lock_guard<std::mutex> lock(mLock);
        mConfigurations[slot] = 0;
        mReady[slot] = false;
        mOwnerClosed[slot] = true;
        if (mSessionOwners[slot] > 0 && mNextIdentity < std::numeric_limits<int64_t>::max()) {
            mSessionOwners[slot] = ++mNextIdentity;
        }
        dropLatestLocked(slot);
    }
    if (mProvider) {
        mProvider->setSlotPresent(slot, present);
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
    releaseFence->set(-1);
    int acq = -1;
    if (!::virtualcamera::duplicateAcquireFence(acquireFence.get(), &acq,
            [](int fd) { return ::dup(fd); })) {
        return ndk::ScopedAStatus::fromServiceSpecificError(5);
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
    if (!::virtualcamera::duplicateAcquireFence(acquireFence.get(), &acq,
            [](int fd) { return ::dup(fd); })) {
        return ndk::ScopedAStatus::fromServiceSpecificError(5);
    }
    int retired = -1;
    auto st = enqueue(slot, buffer, width, height, stride, format, usage,
                      timestampNs, acq, &retired);
    releaseFence->set(retired);
    return st;
}

ndk::ScopedAStatus VirtualCameraStableHal::queueFrameForConfiguration(
        int32_t slot, int64_t configurationToken, const NativeHandle& buffer,
        int32_t width, int32_t height, int32_t stride, int32_t format,
        int64_t usage, int64_t timestampNs, const ndk::ScopedFileDescriptor& acquireFence,
        ndk::ScopedFileDescriptor* releaseFence) {
    releaseFence->set(-1);
    if (!::virtualcamera::validSlot(slot) || configurationToken <= 0) {
        return ndk::ScopedAStatus::fromServiceSpecificError(7);
    }
    int acq = -1;
    if (!::virtualcamera::duplicateAcquireFence(acquireFence.get(), &acq,
            [](int fd) { return ::dup(fd); })) return ndk::ScopedAStatus::fromServiceSpecificError(5);
    int retired = -1;
    auto status = enqueue(slot, buffer, width, height, stride, format, usage,
                          timestampNs, acq, &retired, configurationToken);
    releaseFence->set(retired);
    return status;
}

ndk::ScopedAStatus VirtualCameraStableHal::enqueue(
        int slot, const NativeHandle& buffer, int32_t width, int32_t height,
        int32_t stride, int32_t format, int64_t usage, int64_t timestampNs,
        int acquireFenceFd, int* retiredReadFence, int64_t configurationToken) {
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
        auto admitted = [&] {
            return configurationToken > 0 ? mConfigurations[slot] == configurationToken &&
                    (!mCapabilities[slot] || (mReady[slot] && width==mSourceWidths[slot] && height==mSourceHeights[slot] && format==1)) :
                    mSessionOwners[slot] == 0 && !mProvider && !mCapabilities[slot]; // standalone legacy diagnostics only
        };
        if (!admitted()) {
            AHardwareBuffer_release(ahb); closeFd(acquireFenceFd);
            return ndk::ScopedAStatus::fromServiceSpecificError(7);
        }
        Frame& cur = mLatest[slot];
        if (cur.retiring && !reapRetiringLocked(slot)) {
            AHardwareBuffer_release(ahb);
            closeFd(acquireFenceFd);
            return ndk::ScopedAStatus::fromServiceSpecificError(4);
        }
        if (cur.ahb && cur.inflight > 0) {
            // A capture request is still reading the frame we are about to
            // retire; wait for it to submit and record its read fence.
            if (!mCv.wait_for(lock, kRetireWait,
                              [&cur] { return cur.inflight == 0; })) {
                ALOGW("queueFrame: rejecting new frame with %d old read(s) still in flight",
                      cur.inflight);
                AHardwareBuffer_release(ahb);
                closeFd(acquireFenceFd);
                return ndk::ScopedAStatus::fromServiceSpecificError(4);
            }
        }
        // Close/removal may have happened while the condition wait unlocked.
        if (!admitted()) {
            AHardwareBuffer_release(ahb); closeFd(acquireFenceFd);
            return ndk::ScopedAStatus::fromServiceSpecificError(7);
        }
        if (cur.retiring && !reapRetiringLocked(slot)) {
            AHardwareBuffer_release(ahb);
            closeFd(acquireFenceFd);
            return ndk::ScopedAStatus::fromServiceSpecificError(4);
        }
        if (!consolidateFences(cur.readFences)) {
            // Do not return a partial fence and release the previous buffer.
            AHardwareBuffer_release(ahb);
            closeFd(acquireFenceFd);
            return ndk::ScopedAStatus::fromServiceSpecificError(6);
        }
        if (!cur.readFences.empty()) {
            *retiredReadFence = cur.readFences.back();
            cur.readFences.clear();  // ownership -> caller after successful swap
        }
        old = std::move(cur);
        cur = Frame{};
        cur.ahb = ahb;
        cur.timestampNs = timestampNs;
        cur.acquireFence = acquireFenceFd;
        cur.sequence = ++mFramesReceived;  // never reset on slot close/reconnect
        cur.configurationToken = configurationToken;
        if (mFramesReceived % 150 == 0) {
            ALOGI("queueFrame: %llu frames received (%dx%d, slot %d)%s",
                  (unsigned long long)mFramesReceived, width, height, slot,
                  acquireFenceFd >= 0 ? " [fenced]" : "");
        }
    }
    if (old.ahb) AHardwareBuffer_release(old.ahb);
    closeFd(old.acquireFence);
    return ndk::ScopedAStatus::ok();
}

AHardwareBuffer* VirtualCameraStableHal::acquireLatest(int slot, int64_t* timestampNs,
                                                       int* acquireFenceFd,
                                                       uint64_t* sequence, uint64_t afterSequence,
                                                       int64_t configurationToken) {
    std::lock_guard<std::mutex> lock(mLock);
    if (acquireFenceFd) *acquireFenceFd = -1;
    if (!::virtualcamera::validSlot(slot)) return nullptr;
    Frame& cur = mLatest[slot];
    if (cur.configurationToken != configurationToken ||
            (configurationToken > 0 && mConfigurations[slot] != configurationToken)) return nullptr;
    if (cur.ahb == nullptr || cur.retiring) return nullptr;
    if (sequence && cur.sequence <= afterSequence) return nullptr;
    // Even persistent merge failure cannot accumulate unbounded read fds.
    consolidateFences(cur.readFences);
    if (cur.readFences.size() + cur.inflight >= kMaxPendingReads) return nullptr;
    if (cur.acquireFence >= 0 && (!acquireFenceFd ||
            !::virtualcamera::duplicateAcquireFence(cur.acquireFence, acquireFenceFd,
                    [](int fd) { return ::dup(fd); }))) return nullptr;
    AHardwareBuffer_acquire(cur.ahb);
    cur.inflight++;
    if (timestampNs) *timestampNs = cur.timestampNs;
    if (sequence) *sequence = cur.sequence;
    return cur.ahb;
}

void VirtualCameraStableHal::releaseFrame(int slot, AHardwareBuffer* ahb, int readFenceFd) {
    std::vector<int> fences;
    if (readFenceFd >= 0) fences.push_back(readFenceFd);
    releaseFrame(slot, ahb, std::move(fences));
}

void VirtualCameraStableHal::releaseFrame(int slot, AHardwareBuffer* ahb,
                                         std::vector<int> readFences) {
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (::virtualcamera::validSlot(slot) && mLatest[slot].ahb == ahb) {
            Frame& cur = mLatest[slot];
            cur.readFences.insert(cur.readFences.end(), readFences.begin(), readFences.end());
            readFences.clear();
            consolidateFences(cur.readFences);  // failed merges retain both fds
            if (cur.inflight > 0) cur.inflight--;
            if (cur.retiring) reapRetiringLocked(slot);
            mCv.notify_all();
        }
        // A retiring slot remains identifiable until its final reader returns.
    }
    for (int fd : readFences) if (fd >= 0) ::close(fd);
    if (ahb) AHardwareBuffer_release(ahb);
}

bool VirtualCameraStableHal::hasFrame(int slot) {
    std::lock_guard<std::mutex> lock(mLock);
    return ::virtualcamera::validSlot(slot) && mLatest[slot].ahb != nullptr &&
            !mLatest[slot].retiring;
}

bool VirtualCameraStableHal::isFrameCurrent(int slot, AHardwareBuffer* buffer, uint64_t sequence,
                                           int64_t configurationToken) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!::virtualcamera::validSlot(slot)) return false;
    const auto& frame = mLatest[slot];
    return !frame.retiring && frame.ahb == buffer && frame.sequence == sequence &&
            frame.configurationToken == configurationToken &&
            (configurationToken == 0 || mConfigurations[slot] == configurationToken);
}

int64_t VirtualCameraStableHal::allocateSessionIdentity() {
    std::lock_guard<std::mutex> lock(mLock);
    return mNextIdentity == std::numeric_limits<int64_t>::max() ? 0 : ++mNextIdentity;
}

int64_t VirtualCameraStableHal::notifyStreamsConfigured(
        int slot, int64_t owner, int width, int height, int fps) {
    std::shared_ptr<HalCallback> cb;
    int64_t token, registration=0;
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (!::virtualcamera::validSlot(slot) || owner <= 0 || owner < mSessionOwners[slot] ||
                (owner == mSessionOwners[slot] && mOwnerClosed[slot]) ||
                mNextIdentity == std::numeric_limits<int64_t>::max()) return 0;
        mSessionOwners[slot] = owner;
        mOwnerClosed[slot] = false;
        token = mConfigurations[slot] = ++mNextIdentity;
        mReady[slot] = !mCapabilities[slot];
        mSourceWidths[slot]=width;mSourceHeights[slot]=height;
        dropLatestLocked(slot);
        cb = mCallback;
        if(mCapabilities[slot])registration=mCapabilities[slot]->registration;
    }
    if (!cb) {
        ALOGW("Streams configured (slot %d) but no platform callback registered yet", slot);
        return token;
    }
    if(registration)cb->onStreamsConfiguredForProducer(slot,registration,token,width,height,fps);
    else cb->onStreamsConfiguredForConfiguration(slot, token, width, height, fps);
    return token;
}

void VirtualCameraStableHal::notifyCameraClosed(int slot, int64_t owner, int64_t configurationToken) {
    std::shared_ptr<HalCallback> cb;
    int64_t registration=0;
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (!::virtualcamera::validSlot(slot) || owner <= 0 || configurationToken <= 0 ||
                mSessionOwners[slot] != owner || mConfigurations[slot] != configurationToken) return;
        mConfigurations[slot] = 0;
        mReady[slot] = false;
        mOwnerClosed[slot] = true;
        cb = mCallback;
        if(mCapabilities[slot])registration=mCapabilities[slot]->registration;
        dropLatestLocked(slot);
    }
    if (!cb) return;
    if(registration)cb->onCameraClosedForProducer(slot,registration,configurationToken);
    else cb->onCameraClosedForConfiguration(slot, configurationToken);
}

}  // namespace aidl::android::hardware::camera::provider::implementation

#endif  // VCAM_STABLE_AIDL
