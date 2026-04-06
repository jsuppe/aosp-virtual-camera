/*
 * VirtualCameraDevice - Stub Camera Device (V1 Interface)
 */
#pragma once

#include <aidl/android/hardware/camera/device/BnCameraDevice.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/common/Status.h>
#include <mutex>
#include <string>
#include <memory>

namespace aidl::android::hardware::camera::provider::implementation {

using namespace aidl::android::hardware::camera::device;
using aidl::android::hardware::camera::common::CameraResourceCost;

// Forward declarations
class VirtualCameraFrameSource;
class VirtualCameraFrameSourceV2;

class VirtualCameraDevice : public BnCameraDevice {
public:
    VirtualCameraDevice(const std::string& cameraId,
                        std::shared_ptr<VirtualCameraFrameSource> frameSource,
                        std::shared_ptr<VirtualCameraFrameSourceV2> frameSourceV2);
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
    std::shared_ptr<VirtualCameraFrameSource> mFrameSource;
    std::shared_ptr<VirtualCameraFrameSourceV2> mFrameSourceV2;
    
    void initCharacteristics();
    CameraMetadata mCharacteristics;
};

}  // namespace
