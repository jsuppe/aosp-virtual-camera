/*
 * VirtualCameraProvider - AIDL V1 Adapter Implementation
 */

#define LOG_TAG "VirtualCameraProvider"

#include "VirtualCameraProvider.h"
#include "VirtualCameraDevice.h"
#include "AidlFrameSource.h"
#include "AvailabilityBridge.h"
#include "VendorTags.h"

#include <log/log.h>
#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/common/CameraDeviceStatus.h>

using aidl::android::hardware::camera::common::Status;
using aidl::android::hardware::camera::common::CameraDeviceStatus;

namespace aidl::android::hardware::camera::provider::implementation {

VirtualCameraProvider::VirtualCameraProvider() {
    ALOGI("VirtualCameraProvider created (AIDL V1 adapter)");
    ::virtualcamera::VendorTags::installMetadataOps();

#ifndef VCAM_STABLE_AIDL
    // Create and start the shared FrameSource (v1 - ashmem)
    mFrameSource = std::make_shared<::virtualcamera::VirtualCameraFrameSource>();
    if (mFrameSource->start()) {
        ALOGI("FrameSource v1 socket server started");
    } else {
        ALOGE("Failed to start FrameSource v1 socket server");
    }

    // Create and start v2 zero-copy frame source
    mFrameSourceV2 = std::make_shared<::virtualcamera::VirtualCameraFrameSourceV2>();
    if (mFrameSourceV2->start()) {
        ALOGI("FrameSource v2 (zero-copy) socket server started");
    } else {
        ALOGE("Failed to start FrameSource v2 socket server");
    }
#else
    // Stable-AIDL (vendor APEX) build: frames arrive only through
    // IVirtualCameraHal.queueFrame(). The unix-socket sources bind under
    // /data/local/tmp, which a vendor HAL domain must not touch (and SELinux
    // rightly denies) — so they are not started at all.
#endif

#ifdef VCAM_AIDL_SOURCE
    // Platform relay mode: frames arrive from a producer app registered with
    // VirtualCameraService (system_server) via BufferQueues owned by this HAL.
    mAidlSource = std::make_shared<::virtualcamera::AidlFrameSource>();
    ALOGI("AIDL frame source created (platform relay mode)");

    // Dynamic presence: camera 100 only exists while a producer app is
    // registered with VirtualCameraService. The bridge pushes transitions.
    mAvailBridge = std::make_shared<::virtualcamera::AvailabilityBridge>(
            [this](bool available) { setProducerPresent(available); });
    mAvailBridge->start();
#elif defined(VCAM_STABLE_AIDL)
    // Stable-AIDL build: presence is driven by
    // IVirtualCameraHal.setProducerAvailable() from the platform service.
    ALOGI("Stable-AIDL mode: camera hidden until a producer registers");
#else
    // Legacy socket-only builds: camera is always present.
    mProducerPresent = true;
#endif
}

VirtualCameraProvider::~VirtualCameraProvider() {
#ifdef VCAM_AIDL_SOURCE
    if (mAvailBridge) {
        mAvailBridge->stop();
    }
#endif
    if (mFrameSource) {
        mFrameSource->stop();
    }
    if (mFrameSourceV2) {
        mFrameSourceV2->stop();
    }
    ALOGI("VirtualCameraProvider destroyed");
}

ndk::ScopedAStatus VirtualCameraProvider::setCallback(
        const std::shared_ptr<ICameraProviderCallback>& callback) {
    std::lock_guard<std::mutex> lock(mLock);
    mCallback = callback;
    ALOGI("Provider callback set");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraProvider::getVendorTags(
        std::vector<common::VendorTagSection>* vendorTags) {
    if (!vendorTags) return ndk::ScopedAStatus::ok();
    vendorTags->clear();
    common::VendorTagSection section;
    section.sectionName = ::virtualcamera::VendorTags::sectionName();
    size_t n = 0;
    const auto* defs = ::virtualcamera::VendorTags::tags(&n);
    for (size_t i = 0; i < n; i++) {
        common::VendorTag tag;
        tag.tagId = static_cast<int32_t>(defs[i].id);
        tag.tagName = defs[i].name;
        tag.tagType = static_cast<common::CameraMetadataType>(defs[i].type);
        section.tags.push_back(tag);
    }
    vendorTags->push_back(std::move(section));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraProvider::getCameraIdList(
        std::vector<std::string>* cameraIds) {
    if (cameraIds) {
        cameraIds->clear();
        if (mProducerPresent.load(std::memory_order_acquire)) {
            cameraIds->push_back(kVirtualCameraId);
            ALOGI("Returning camera list with: %s", kVirtualCameraId);
        } else {
            ALOGI("No producer registered - returning empty camera list");
        }
    }
    return ndk::ScopedAStatus::ok();
}

void VirtualCameraProvider::setProducerPresent(bool present) {
    bool prev = mProducerPresent.exchange(present, std::memory_order_acq_rel);
    if (prev == present) {
        return;
    }
    std::shared_ptr<ICameraProviderCallback> cb;
    {
        std::lock_guard<std::mutex> lock(mLock);
        cb = mCallback;
    }
    ALOGI("Virtual camera %s -> %s", kVirtualCameraId,
          present ? "PRESENT" : "NOT_PRESENT");
    if (cb) {
        auto status = cb->cameraDeviceStatusChange(
                std::string(kVirtualCameraId),
                present ? CameraDeviceStatus::PRESENT
                        : CameraDeviceStatus::NOT_PRESENT);
        if (!status.isOk()) {
            ALOGW("cameraDeviceStatusChange failed");
        }
    }
}

ndk::ScopedAStatus VirtualCameraProvider::getCameraDeviceInterface(
        const std::string& cameraDeviceId,
        std::shared_ptr<device::ICameraDevice>* device) {

    if (cameraDeviceId != kVirtualCameraId) {
        ALOGE("Unknown camera ID: %s", cameraDeviceId.c_str());
        return ndk::ScopedAStatus::fromServiceSpecificError(
                static_cast<int32_t>(Status::ILLEGAL_ARGUMENT));
    }

    ALOGI("Creating device interface for: %s", cameraDeviceId.c_str());
    *device = ndk::SharedRefBase::make<VirtualCameraDevice>(
        cameraDeviceId, mFrameSource, mFrameSourceV2, mAidlSource);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraProvider::notifyDeviceStateChange(int64_t deviceState) {
    ALOGI("Device state changed: %ld", (long)deviceState);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraProvider::getConcurrentCameraIds(
        std::vector<ConcurrentCameraIdCombination>* concurrentCameraIds) {
    if (concurrentCameraIds) {
        concurrentCameraIds->clear();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraProvider::isConcurrentStreamCombinationSupported(
        const std::vector<CameraIdAndStreamCombination>& /*configs*/,
        bool* supported) {
    if (supported) {
        *supported = false;
    }
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::camera::provider::implementation
