/*
 * VirtualCameraDevice - Implementation with VirtualCameraSession
 */

#define LOG_TAG "VirtualCameraDevice"

#include "VirtualCameraDevice.h"
#include "VirtualCameraSession.h"
#include "VirtualCameraFrameSourceV2.h"

#include <log/log.h>
#include <system/camera_metadata.h>
#include <system/graphics.h>
#include <aidl/android/hardware/camera/common/Status.h>

namespace aidl::android::hardware::camera::provider::implementation {

using aidl::android::hardware::camera::common::Status;

VirtualCameraDevice::VirtualCameraDevice(const std::string& cameraId,
                                         std::shared_ptr<VirtualCameraFrameSource> frameSource,
                                         std::shared_ptr<VirtualCameraFrameSourceV2> frameSourceV2)
    : mCameraId(cameraId), mFrameSource(frameSource), mFrameSourceV2(frameSourceV2) {
    ALOGI("VirtualCameraDevice created: %s", cameraId.c_str());
    initCharacteristics();
}

void VirtualCameraDevice::initCharacteristics() {
    // Create camera characteristics with enough space for all entries
    camera_metadata_t* meta = allocate_camera_metadata(100, 2000);
    
    // Required characteristics for a basic camera
    uint8_t facing = ANDROID_LENS_FACING_EXTERNAL;
    add_camera_metadata_entry(meta, ANDROID_LENS_FACING, &facing, 1);
    
    int32_t orientation = 0;
    add_camera_metadata_entry(meta, ANDROID_SENSOR_ORIENTATION, &orientation, 1);
    
    // Supported stream configurations: IMPLEMENTATION_DEFINED, RGBA and YUV_420_888
    const int32_t streamConfigs[] = {
        // IMPLEMENTATION_DEFINED (required for SurfaceView preview)
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1920, 1080,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 640, 480,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        // RGBA formats
        HAL_PIXEL_FORMAT_RGBA_8888, 1920, 1080,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_RGBA_8888, 1280, 720,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_RGBA_8888, 640, 480,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        // YUV_420_888 formats (required for most camera apps)
        HAL_PIXEL_FORMAT_YCbCr_420_888, 1920, 1080,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_YCbCr_420_888, 1280, 720,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
        HAL_PIXEL_FORMAT_YCbCr_420_888, 640, 480,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT,
    };
    constexpr size_t streamConfigsCount = 36;  // 9 configs * 4 entries each
    add_camera_metadata_entry(meta, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                              streamConfigs, streamConfigsCount);
    
    // Min frame durations (33ms = 30fps)
    const int64_t frameDurations[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1920, 1080, 33333333LL,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 640, 480, 33333333LL,
        HAL_PIXEL_FORMAT_RGBA_8888, 1920, 1080, 33333333LL,
        HAL_PIXEL_FORMAT_RGBA_8888, 1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_RGBA_8888, 640, 480, 33333333LL,
        HAL_PIXEL_FORMAT_YCbCr_420_888, 1920, 1080, 33333333LL,
        HAL_PIXEL_FORMAT_YCbCr_420_888, 1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_YCbCr_420_888, 640, 480, 33333333LL,
    };
    constexpr size_t frameDurationsCount = 36;  // 9 configs * 4 entries each
    add_camera_metadata_entry(meta, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                              frameDurations, frameDurationsCount);
    
    // Stall durations (0 for all formats - we don't stall)
    const int64_t stallDurations[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1920, 1080, 0LL,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, 0LL,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 640, 480, 0LL,
        HAL_PIXEL_FORMAT_RGBA_8888, 1920, 1080, 0LL,
        HAL_PIXEL_FORMAT_RGBA_8888, 1280, 720, 0LL,
        HAL_PIXEL_FORMAT_RGBA_8888, 640, 480, 0LL,
        HAL_PIXEL_FORMAT_YCbCr_420_888, 1920, 1080, 0LL,
        HAL_PIXEL_FORMAT_YCbCr_420_888, 1280, 720, 0LL,
        HAL_PIXEL_FORMAT_YCbCr_420_888, 640, 480, 0LL,
    };
    constexpr size_t stallDurationsCount = 36;  // 9 configs * 4 entries each
    add_camera_metadata_entry(meta, ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
                              stallDurations, stallDurationsCount);
    
    // Hardware level
    uint8_t hwLevel = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL;
    add_camera_metadata_entry(meta, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &hwLevel, 1);
    
    // Available capabilities (required!)
    const uint8_t capabilities[] = {
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE,
    };
    add_camera_metadata_entry(meta, ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
                              capabilities, sizeof(capabilities));
    
    // Partial result count (we send single complete result)
    int32_t partialResultCount = 1;
    add_camera_metadata_entry(meta, ANDROID_REQUEST_PARTIAL_RESULT_COUNT, 
                              &partialResultCount, 1);
    
    // Zoom ratio range (1.0x only - no zoom)
    const float zoomRange[] = {1.0f, 1.0f};
    add_camera_metadata_entry(meta, ANDROID_CONTROL_ZOOM_RATIO_RANGE, zoomRange, 2);
    
    // Active array size (required)
    const int32_t activeArray[] = {0, 0, 1920, 1080};
    add_camera_metadata_entry(meta, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, activeArray, 4);
    
    // Max digital zoom
    const float maxZoom = 1.0f;
    add_camera_metadata_entry(meta, ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &maxZoom, 1);
    
    // Scaler crop region
    const int32_t cropRegion[] = {0, 0, 1920, 1080};
    add_camera_metadata_entry(meta, ANDROID_SCALER_CROP_REGION, cropRegion, 4);
    
    // Available request keys
    const int32_t requestKeys[] = {
        ANDROID_CONTROL_MODE,
        ANDROID_CONTROL_AE_MODE,
        ANDROID_CONTROL_AWB_MODE,
    };
    add_camera_metadata_entry(meta, ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, 
                              requestKeys, sizeof(requestKeys)/sizeof(int32_t));
    
    // Available result keys
    const int32_t resultKeys[] = {
        ANDROID_SENSOR_TIMESTAMP,
        ANDROID_CONTROL_ZOOM_RATIO,
    };
    add_camera_metadata_entry(meta, ANDROID_REQUEST_AVAILABLE_RESULT_KEYS,
                              resultKeys, sizeof(resultKeys)/sizeof(int32_t));
    
    // Available characteristics keys
    const int32_t charKeys[] = {
        ANDROID_LENS_FACING,
        ANDROID_SENSOR_ORIENTATION,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
        ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
        ANDROID_CONTROL_ZOOM_RATIO_RANGE,
    };
    add_camera_metadata_entry(meta, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
                              charKeys, sizeof(charKeys)/sizeof(int32_t));
    
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
    
    *_aidl_return = ndk::SharedRefBase::make<VirtualCameraSession>(
        callback, mFrameSource, mFrameSourceV2);
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
