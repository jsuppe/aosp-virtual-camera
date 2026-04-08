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
    camera_metadata_t* meta = allocate_camera_metadata(100, 4000);
    
    // Required characteristics for a basic camera
    uint8_t facing = ANDROID_LENS_FACING_EXTERNAL;
    add_camera_metadata_entry(meta, ANDROID_LENS_FACING, &facing, 1);
    
    int32_t orientation = 0;
    add_camera_metadata_entry(meta, ANDROID_SENSOR_ORIENTATION, &orientation, 1);
    
    // Supported resolutions: 4K, 1080p, 720p, 480p
    // Each format × resolution is a 4-entry tuple
    struct Res { int32_t w, h; };
    static constexpr Res kResolutions[] = {
        {3840, 2160}, {1920, 1080}, {1280, 720}, {640, 480},
    };
    static constexpr int32_t kFormats[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
        HAL_PIXEL_FORMAT_RGBA_8888,
        HAL_PIXEL_FORMAT_YCbCr_420_888,
    };
    static constexpr int kNumRes = sizeof(kResolutions) / sizeof(kResolutions[0]);
    static constexpr int kNumFmt = sizeof(kFormats) / sizeof(kFormats[0]);
    static constexpr int kNumConfigs = kNumFmt * kNumRes;

    // 16666666ns = 60fps
    static constexpr int64_t kMinFrameDurationNs = 16666666LL;

    // Stream configurations
    int32_t streamConfigs[kNumConfigs * 4];
    int64_t frameDurations[kNumConfigs * 4];
    int64_t stallDurations[kNumConfigs * 4];
    int idx = 0;
    for (int f = 0; f < kNumFmt; f++) {
        for (int r = 0; r < kNumRes; r++) {
            int base = idx * 4;
            streamConfigs[base + 0] = kFormats[f];
            streamConfigs[base + 1] = kResolutions[r].w;
            streamConfigs[base + 2] = kResolutions[r].h;
            streamConfigs[base + 3] = ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT;

            frameDurations[base + 0] = kFormats[f];
            frameDurations[base + 1] = kResolutions[r].w;
            frameDurations[base + 2] = kResolutions[r].h;
            frameDurations[base + 3] = kMinFrameDurationNs;

            stallDurations[base + 0] = kFormats[f];
            stallDurations[base + 1] = kResolutions[r].w;
            stallDurations[base + 2] = kResolutions[r].h;
            stallDurations[base + 3] = 0LL;
            idx++;
        }
    }
    add_camera_metadata_entry(meta, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                              streamConfigs, kNumConfigs * 4);
    add_camera_metadata_entry(meta, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                              frameDurations, kNumConfigs * 4);
    add_camera_metadata_entry(meta, ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
                              stallDurations, kNumConfigs * 4);
    
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
    
    // Active array size = max resolution (4K)
    const int32_t activeArray[] = {0, 0, 3840, 2160};
    add_camera_metadata_entry(meta, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, activeArray, 4);

    // Max digital zoom
    const float maxZoom = 1.0f;
    add_camera_metadata_entry(meta, ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &maxZoom, 1);

    // Scaler crop region = full sensor (4K)
    const int32_t cropRegion[] = {0, 0, 3840, 2160};
    add_camera_metadata_entry(meta, ANDROID_SCALER_CROP_REGION, cropRegion, 4);

    // AE available FPS ranges
    const int32_t fpsRanges[] = {
        15, 30,   // 15-30 fps (power saving / variable)
        30, 30,   // locked 30 fps
        30, 60,   // 30-60 fps
        60, 60,   // locked 60 fps
    };
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                              fpsRanges, sizeof(fpsRanges) / sizeof(int32_t));
    
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
