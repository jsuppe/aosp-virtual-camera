/*
 * VirtualCameraDevice - Implementation with VirtualCameraSession
 */

#define LOG_TAG "VirtualCameraDevice"

#include "VirtualCameraDevice.h"
#include "VirtualCameraSession.h"

#include <log/log.h>
#include <system/camera_metadata.h>
#include <system/graphics.h>
#include <aidl/android/hardware/camera/common/Status.h>

namespace aidl::android::hardware::camera::provider::implementation {

using aidl::android::hardware::camera::common::Status;

VirtualCameraDevice::VirtualCameraDevice(const std::string& cameraId)
    : mCameraId(cameraId) {
    ALOGI("VirtualCameraDevice created: %s", cameraId.c_str());
    initCharacteristics();
}

void VirtualCameraDevice::initCharacteristics() {
    // Create minimal camera characteristics
    camera_metadata_t* meta = allocate_camera_metadata(50, 500);
    
    // Required characteristics for a basic camera
    uint8_t facing = ANDROID_LENS_FACING_EXTERNAL;
    add_camera_metadata_entry(meta, ANDROID_LENS_FACING, &facing, 1);
    
    int32_t orientation = 0;
    add_camera_metadata_entry(meta, ANDROID_SENSOR_ORIENTATION, &orientation, 1);
    
    // Supported stream configurations: 1080p RGBA
    const int32_t streamConfigs[] = {
        HAL_PIXEL_FORMAT_RGBA_8888, 1920, 1080,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_RGBA_8888, 1280, 720,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_RGBA_8888, 640, 480,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
    };
    constexpr size_t streamConfigsCount = 12;  // 3 resolutions * 4 entries each
    add_camera_metadata_entry(meta, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                              streamConfigs, streamConfigsCount);
    
    // Min frame durations (33ms = 30fps)
    const int64_t frameDurations[] = {
        HAL_PIXEL_FORMAT_RGBA_8888, 1920, 1080, 33333333LL,
        HAL_PIXEL_FORMAT_RGBA_8888, 1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_RGBA_8888, 640, 480, 33333333LL,
    };
    constexpr size_t frameDurationsCount = 12;  // 3 resolutions * 4 entries each
    add_camera_metadata_entry(meta, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                              frameDurations, frameDurationsCount);
    
    // Hardware level
    uint8_t hwLevel = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL;
    add_camera_metadata_entry(meta, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &hwLevel, 1);
    
    // Copy to our metadata vector
    size_t metaSize = get_camera_metadata_size(meta);
    mCharacteristics.metadata.resize(metaSize);
    memcpy(mCharacteristics.metadata.data(), meta, metaSize);
    
    free_camera_metadata(meta);
    ALOGI("Camera characteristics initialized");
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
        const StreamConfiguration& /*streams*/,
        bool* _aidl_return) {
    // Accept any reasonable stream config for now
    if (_aidl_return) {
        *_aidl_return = true;
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
    
    *_aidl_return = ndk::SharedRefBase::make<VirtualCameraSession>(callback);
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

}  // namespace
