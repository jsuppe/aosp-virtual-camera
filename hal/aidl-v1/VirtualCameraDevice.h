/*
 * VirtualCameraDevice - AIDL V1 Camera Device Adapter
 *
 * Thin adapter implementing BnCameraDevice. Delegates metadata building
 * to core::MetadataBuilder and creates V1 sessions.
 */
#pragma once

#include <aidl/android/hardware/camera/device/BnCameraDevice.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/common/Status.h>
#include <mutex>
#include <string>
#include <memory>

// Core types
#include "VirtualCameraFrameSource.h"
#include "VirtualCameraFrameSourceV2.h"
#include "MetadataBuilder.h"

namespace aidl::android::hardware::camera::provider::implementation {

using namespace aidl::android::hardware::camera::device;
using aidl::android::hardware::camera::common::CameraResourceCost;

class VirtualCameraDevice : public BnCameraDevice {
public:
    VirtualCameraDevice(const std::string& cameraId,
                        std::shared_ptr<virtualcamera::VirtualCameraFrameSource> frameSource,
                        std::shared_ptr<virtualcamera::VirtualCameraFrameSourceV2> frameSourceV2);
    ~VirtualCameraDevice() override = default;

    // ICameraDevice V1 interface
    ndk::ScopedAStatus getCameraCharacteristics(
            CameraMetadata* _aidl_return) override;

    ndk::ScopedAStatus getPhysicalCameraCharacteristics(
            const std::string& physicalCameraId,
            CameraMetadata* _aidl_return) override;

    ndk::ScopedAStatus getResourceCost(
            CameraResourceCost* _aidl_return) override;

    ndk::ScopedAStatus isStreamCombinationSupported(
            const StreamConfiguration& streams,
            bool* _aidl_return) override;

    ndk::ScopedAStatus open(
            const std::shared_ptr<ICameraDeviceCallback>& callback,
            std::shared_ptr<ICameraDeviceSession>* _aidl_return) override;

    ndk::ScopedAStatus openInjectionSession(
            const std::shared_ptr<ICameraDeviceCallback>& callback,
            std::shared_ptr<ICameraInjectionSession>* _aidl_return) override;

    ndk::ScopedAStatus setTorchMode(bool on) override;

    ndk::ScopedAStatus turnOnTorchWithStrengthLevel(int32_t torchStrength) override;

    ndk::ScopedAStatus getTorchStrengthLevel(int32_t* _aidl_return) override;

private:
    std::string mCameraId;
    std::mutex mLock;
    std::shared_ptr<virtualcamera::VirtualCameraFrameSource> mFrameSource;
    std::shared_ptr<virtualcamera::VirtualCameraFrameSourceV2> mFrameSourceV2;

    CameraMetadata mCharacteristics;
};

}  // namespace aidl::android::hardware::camera::provider::implementation
