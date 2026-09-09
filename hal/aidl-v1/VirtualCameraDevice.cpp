/*
 * VirtualCameraDevice - AIDL V1 Adapter Implementation
 *
 * Delegates characteristics building to core::MetadataBuilder.
 */

#define LOG_TAG "VirtualCameraDevice"

#include "VirtualCameraDevice.h"
#include "MetadataBuilder.h"

#include <vector>
#include "VirtualCameraSession.h"

#include <log/log.h>
#include <aidl/android/hardware/camera/common/Status.h>

namespace aidl::android::hardware::camera::provider::implementation {

using aidl::android::hardware::camera::common::Status;

VirtualCameraDevice::VirtualCameraDevice(
        const std::string& cameraId,
        std::shared_ptr<::virtualcamera::VirtualCameraFrameSource> frameSource,
        std::shared_ptr<::virtualcamera::VirtualCameraFrameSourceV2> frameSourceV2,
        std::shared_ptr<::virtualcamera::AidlFrameSource> aidlSource)
    : mCameraId(cameraId), mFrameSource(frameSource), mFrameSourceV2(frameSourceV2),
      mAidlSource(aidlSource) {
    ALOGI("VirtualCameraDevice created: %s", cameraId.c_str());
    mCharacteristics.metadata = ::virtualcamera::MetadataBuilder::buildCameraCharacteristics();
}

ndk::ScopedAStatus VirtualCameraDevice::getCameraCharacteristics(
        CameraMetadata* _aidl_return) {
    if (_aidl_return) {
        *_aidl_return = mCharacteristics;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraDevice::getPhysicalCameraCharacteristics(
        const std::string& /*physicalCameraId*/,
        CameraMetadata* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
            static_cast<int32_t>(Status::ILLEGAL_ARGUMENT));
}

ndk::ScopedAStatus VirtualCameraDevice::getResourceCost(
        CameraResourceCost* _aidl_return) {
    if (_aidl_return) {
        _aidl_return->resourceCost = 50;  // Low cost
        _aidl_return->conflictingDevices.clear();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraDevice::isStreamCombinationSupported(
        const StreamConfiguration& streams,
        bool* _aidl_return) {
    if (_aidl_return) {
        *_aidl_return = ::virtualcamera::MetadataBuilder::isStreamCombinationSupported(
                [&] {
                    std::vector<::virtualcamera::MetadataBuilder::StreamDesc> v;
                    for (const auto& s : streams.streams) {
                        v.push_back({static_cast<int>(s.format), s.width, s.height,
                                     static_cast<int64_t>(s.useCase),
                                     s.streamType == ::aidl::android::hardware::camera::device::StreamType::INPUT,
                                     static_cast<int>(s.rotation)});
                    }
                    return v;
                }());
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraDevice::open(
        const std::shared_ptr<ICameraDeviceCallback>& callback,
        std::shared_ptr<ICameraDeviceSession>* _aidl_return) {
    ALOGI("Camera open requested for %s", mCameraId.c_str());

    if (!callback) {
        ALOGE("Callback is null");
        return ndk::ScopedAStatus::fromServiceSpecificError(
                static_cast<int32_t>(Status::ILLEGAL_ARGUMENT));
    }

    *_aidl_return = ndk::SharedRefBase::make<VirtualCameraSession>(
        callback, mFrameSource, mFrameSourceV2, mAidlSource);
    ALOGI("Camera session created successfully");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraDevice::openInjectionSession(
        const std::shared_ptr<ICameraDeviceCallback>& /*callback*/,
        std::shared_ptr<ICameraInjectionSession>* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
            static_cast<int32_t>(Status::OPERATION_NOT_SUPPORTED));
}

ndk::ScopedAStatus VirtualCameraDevice::setTorchMode(bool /*on*/) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
            static_cast<int32_t>(Status::OPERATION_NOT_SUPPORTED));
}

ndk::ScopedAStatus VirtualCameraDevice::turnOnTorchWithStrengthLevel(int32_t /*torchStrength*/) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
            static_cast<int32_t>(Status::OPERATION_NOT_SUPPORTED));
}

ndk::ScopedAStatus VirtualCameraDevice::getTorchStrengthLevel(int32_t* /*_aidl_return*/) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
            static_cast<int32_t>(Status::OPERATION_NOT_SUPPORTED));
}

}  // namespace aidl::android::hardware::camera::provider::implementation
