/*
 * AvailabilityBridge implementation (A13 platform relay).
 */

#ifdef VCAM_AIDL_SOURCE

#define LOG_TAG "VCamAvailBridge"

#include "AvailabilityBridge.h"

#include <android/hardware/virtualcamera/BnVirtualCameraHalListener.h>
#include <android/hardware/virtualcamera/IVirtualCameraManager.h>
#include <binder/IServiceManager.h>
#include <log/log.h>

#include <chrono>

namespace virtualcamera {

namespace vcaidl = ::android::hardware::virtualcamera;
using ::android::IBinder;
using ::android::interface_cast;
using ::android::sp;
using ::android::String16;

namespace {

class ListenerImpl : public vcaidl::BnVirtualCameraHalListener {
public:
    explicit ListenerImpl(AvailabilityBridge* bridge) : mBridge(bridge) {}
    ::android::binder::Status onProducerAvailabilityChanged(bool available) override {
        mBridge->onAvailability(available);
        return ::android::binder::Status::ok();
    }
private:
    AvailabilityBridge* const mBridge;
};

class DeathRecipientImpl : public IBinder::DeathRecipient {
public:
    explicit DeathRecipientImpl(AvailabilityBridge* bridge) : mBridge(bridge) {}
    void binderDied(const ::android::wp<IBinder>& /*who*/) override {
        mBridge->onManagerDied();
    }
private:
    AvailabilityBridge* const mBridge;
};

}  // namespace

AvailabilityBridge::AvailabilityBridge(Callback cb) : mCallback(std::move(cb)) {}

AvailabilityBridge::~AvailabilityBridge() {
    stop();
}

void AvailabilityBridge::start() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mThread.joinable()) return;
    mStop = false;
    mThread = std::thread([this] { threadLoop(); });
}

void AvailabilityBridge::stop() {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mStop = true;
    }
    mCv.notify_all();
    if (mThread.joinable()) mThread.join();
}

void AvailabilityBridge::onAvailability(bool available) {
    ALOGI("Producer availability -> %s", available ? "AVAILABLE" : "UNAVAILABLE");
    if (mCallback) mCallback(available);
}

void AvailabilityBridge::onManagerDied() {
    ALOGW("virtual_camera_manager died; treating producers as unavailable");
    {
        std::lock_guard<std::mutex> lock(mLock);
        mConnected = false;
        mManager = nullptr;
        mListener = nullptr;
    }
    if (mCallback) mCallback(false);
    mCv.notify_all();  // wake the loop to reconnect
}

bool AvailabilityBridge::connectLocked() {
    sp<IBinder> binder = ::android::defaultServiceManager()->checkService(
            String16("virtual_camera_manager"));
    if (binder == nullptr) {
        return false;  // system_server / service not up yet
    }
    sp<vcaidl::IVirtualCameraManager> mgr =
            interface_cast<vcaidl::IVirtualCameraManager>(binder);
    if (mgr == nullptr) {
        return false;
    }

    if (mDeathRecipient == nullptr) {
        mDeathRecipient = sp<DeathRecipientImpl>::make(this);
    }
    binder->linkToDeath(mDeathRecipient);

    sp<ListenerImpl> listener = sp<ListenerImpl>::make(this);
    // setHalListener synchronously pushes the current availability through
    // onProducerAvailabilityChanged before returning.
    ::android::binder::Status status = mgr->setHalListener(listener);
    if (!status.isOk()) {
        ALOGW("setHalListener failed: %s", status.toString8().c_str());
        binder->unlinkToDeath(mDeathRecipient);
        return false;
    }

    mManager = mgr;
    mListener = listener;
    mConnected = true;
    ALOGI("Registered HAL availability listener with virtual_camera_manager");
    return true;
}

void AvailabilityBridge::threadLoop() {
    std::unique_lock<std::mutex> lock(mLock);
    while (!mStop) {
        if (!mConnected) {
            connectLocked();
        }
        // Connected: sleep until death/stop. Not connected: retry in 2s.
        mCv.wait_for(lock, std::chrono::seconds(mConnected ? 3600 : 2),
                     [this] { return mStop || !mConnected; });
    }
}

}  // namespace virtualcamera

#endif  // VCAM_AIDL_SOURCE
