/*
 * VirtualCameraProvider - Stub Implementation
 */

#define LOG_TAG "VirtualCameraProvider"

#include "VirtualCameraProvider.h"
#include "VirtualCameraDevice.h"
#include "VirtualCameraFrameSource.h"

#include <log/log.h>
#include <aidl/android/hardware/camera/common/Status.h>

using aidl::android::hardware::camera::common::Status;

namespace aidl::android::hardware::camera::provider::implementation {

VirtualCameraProvider::VirtualCameraProvider() {
    ALOGI("VirtualCameraProvider created");
    
    // Create and start the shared FrameSource
    mFrameSource = std::make_shared<VirtualCameraFrameSource>();
    if (mFrameSource->start()) {
        ALOGI("FrameSource socket server started");
    } else {
        ALOGE("Failed to start FrameSource socket server");
    }
}

VirtualCameraProvider::~VirtualCameraProvider() {
    if (mFrameSource) {
        mFrameSource->stop();
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
    if (vendorTags) {
        vendorTags->clear();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraProvider::getCameraIdList(
        std::vector<std::string>* cameraIds) {
    if (cameraIds) {
        cameraIds->clear();
        cameraIds->push_back(kVirtualCameraId);
        ALOGI("Returning camera list with: %s", kVirtualCameraId);
    }
    return ndk::ScopedAStatus::ok();
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
    *device = ndk::SharedRefBase::make<VirtualCameraDevice>(cameraDeviceId, mFrameSource);
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

}  // namespace
