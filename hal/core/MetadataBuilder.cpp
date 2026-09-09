/*
 * MetadataBuilder - Camera metadata construction utilities
 */

#define LOG_TAG "VCamMetadataBuilder"

#include "MetadataBuilder.h"
#include "VendorTags.h"

#include <log/log.h>
#include <system/camera_metadata.h>
#include <system/graphics.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace virtualcamera {

std::vector<uint8_t> MetadataBuilder::buildCameraCharacteristics() {
    // Create camera characteristics with enough space for all entries
    camera_metadata_t* meta = allocate_camera_metadata(120, 6000);

    // Required characteristics for a basic camera
    uint8_t facing = ANDROID_LENS_FACING_EXTERNAL;
    add_camera_metadata_entry(meta, ANDROID_LENS_FACING, &facing, 1);

    int32_t orientation = 0;
    add_camera_metadata_entry(meta, ANDROID_SENSOR_ORIENTATION, &orientation, 1);

    // Supported resolutions: 4K, 1080p, 720p, 480p
    // Each format x resolution is a 4-entry tuple
    struct Res { int32_t w, h; };
    static constexpr Res kResolutions[] = {
        {3840, 2160}, {1920, 1080}, {1280, 720}, {640, 480},
    };
    static constexpr int32_t kFormats[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
        HAL_PIXEL_FORMAT_RGBA_8888,
        HAL_PIXEL_FORMAT_YCbCr_420_888,
        HAL_PIXEL_FORMAT_BLOB,          // JPEG stills (JpegEncoder)
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
            // BLOB is a stalling format: CPU JPEG encode of a 4K frame.
            stallDurations[base + 3] = (kFormats[f] == HAL_PIXEL_FORMAT_BLOB) ? 100000000LL : 0LL;
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

    // 3A control: we have no 3A, so OFF and AUTO behave identically, but the
    // key set must be complete for BACKWARD_COMPATIBLE (VTS checks it).
    const uint8_t controlModes[] = { ANDROID_CONTROL_MODE_OFF, ANDROID_CONTROL_MODE_AUTO };
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AVAILABLE_MODES, controlModes, 2);
    const uint8_t aeModes[] = { ANDROID_CONTROL_AE_MODE_ON };
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AE_AVAILABLE_MODES, aeModes, 1);
    const uint8_t awbModes[] = { ANDROID_CONTROL_AWB_MODE_AUTO };
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AWB_AVAILABLE_MODES, awbModes, 1);
    const uint8_t afModes[] = { ANDROID_CONTROL_AF_MODE_OFF };
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AF_AVAILABLE_MODES, afModes, 1);

    uint8_t croppingType = ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY;
    add_camera_metadata_entry(meta, ANDROID_SCALER_CROPPING_TYPE, &croppingType, 1);

    // JPEG (BLOB) output
    int32_t jpegMaxSize = kJpegMaxSize;
    add_camera_metadata_entry(meta, ANDROID_JPEG_MAX_SIZE, &jpegMaxSize, 1);
    const int32_t thumbSizes[] = { 0, 0 };   // no thumbnails
    add_camera_metadata_entry(meta, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, thumbSizes, 2);

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
        ANDROID_CONTROL_AE_TARGET_FPS_RANGE,   // drives the request-loop pacing
        ANDROID_CONTROL_ZOOM_RATIO,            // zoom keys: all four or none (VTS)
        ANDROID_JPEG_QUALITY,
        ANDROID_JPEG_ORIENTATION,
    };
    add_camera_metadata_entry(meta, ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS,
                              requestKeys, sizeof(requestKeys)/sizeof(int32_t));

    // Available result keys
    const int32_t resultKeys[] = {
        ANDROID_SENSOR_TIMESTAMP,
        ANDROID_SENSOR_FRAME_DURATION,
        ANDROID_CONTROL_ZOOM_RATIO,
        ANDROID_SCALER_CROP_REGION,
        static_cast<int32_t>(VendorTags::kProducerTimestampNs),
    };
    add_camera_metadata_entry(meta, ANDROID_REQUEST_AVAILABLE_RESULT_KEYS,
                              resultKeys, sizeof(resultKeys)/sizeof(int32_t));

    // Available characteristics keys
    const int32_t charKeys[] = {
        ANDROID_LENS_FACING,
        ANDROID_SENSOR_ORIENTATION,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
        ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
        ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
        ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
        ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
        ANDROID_CONTROL_AVAILABLE_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_MODES,
        ANDROID_CONTROL_AWB_AVAILABLE_MODES,
        ANDROID_CONTROL_AF_AVAILABLE_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
        ANDROID_CONTROL_ZOOM_RATIO_RANGE,
        ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
        ANDROID_SCALER_CROPPING_TYPE,
        ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
        ANDROID_JPEG_MAX_SIZE,
        ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
    };
    add_camera_metadata_entry(meta, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
                              charKeys, sizeof(charKeys)/sizeof(int32_t));

    // Serialize to byte vector
    size_t metaSize = get_camera_metadata_size(meta);
    std::vector<uint8_t> result(metaSize);
    memcpy(result.data(), meta, metaSize);
    free_camera_metadata(meta);

    ALOGI("Camera characteristics built: %zu bytes", metaSize);
    return result;
}

bool MetadataBuilder::isStreamCombinationSupported(const std::vector<StreamDesc>& streams) {
    if (streams.empty()) return false;
    static const int kFormats[] = { HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
                                    HAL_PIXEL_FORMAT_RGBA_8888,
                                    HAL_PIXEL_FORMAT_YCbCr_420_888,
                                    HAL_PIXEL_FORMAT_BLOB };
    static const int kSizes[][2] = { {3840, 2160}, {1920, 1080}, {1280, 720}, {640, 480} };
    for (const auto& s : streams) {
        if (s.input) return false;                         // no reprocessing
        if (s.useCase != 0 /* ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT */) {
            return false;                                  // no use cases advertised
        }
        if (s.rotation < 0 || s.rotation > 3) return false;   // StreamRotation enum range
        bool fmt = false, size = false;
        for (int f : kFormats) fmt |= (f == s.format);
        for (const auto& sz : kSizes) size |= (sz[0] == s.width && sz[1] == s.height);
        if (!fmt || !size) return false;
    }
    return true;
}

std::vector<uint8_t> MetadataBuilder::buildDefaultRequestSettings() {
    camera_metadata_t* meta = allocate_camera_metadata(10, 200);

    uint8_t controlMode = ANDROID_CONTROL_MODE_AUTO;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_MODE, &controlMode, 1);

    uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AE_MODE, &aeMode, 1);

    uint8_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
    const int32_t fpsRange[] = { 30, 30 };
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, fpsRange, 2);
    uint8_t jpegQuality = 90;
    add_camera_metadata_entry(meta, ANDROID_JPEG_QUALITY, &jpegQuality, 1);
    int32_t jpegOrientation = 0;
    add_camera_metadata_entry(meta, ANDROID_JPEG_ORIENTATION, &jpegOrientation, 1);

    float zoomRatio = 1.0f;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_ZOOM_RATIO, &zoomRatio, 1);

    size_t metaSize = get_camera_metadata_size(meta);
    std::vector<uint8_t> result(metaSize);
    memcpy(result.data(), meta, metaSize);
    free_camera_metadata(meta);

    return result;
}

std::vector<uint8_t> MetadataBuilder::buildResultMetadata(int64_t timestamp,
                                                          int64_t frameDurationNs,
                                                          int64_t producerTimestampNs) {
    camera_metadata_t* meta = allocate_camera_metadata(10, 200);

    int64_t ts = timestamp;
    add_camera_metadata_entry(meta, ANDROID_SENSOR_TIMESTAMP, &ts, 1);
    add_camera_metadata_entry(meta, ANDROID_SENSOR_FRAME_DURATION, &frameDurationNs, 1);
    if (producerTimestampNs > 0) {
        // Vendor tag (VendorTags.h): needs installMetadataOps() once per process.
        add_camera_metadata_entry(meta, VendorTags::kProducerTimestampNs,
                                  &producerTimestampNs, 1);
    }

    float zoomRatio = 1.0f;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_ZOOM_RATIO, &zoomRatio, 1);

    int32_t cropRegion[] = {0, 0, 3840, 2160};
    add_camera_metadata_entry(meta, ANDROID_SCALER_CROP_REGION, cropRegion, 4);

    size_t metaSize = get_camera_metadata_size(meta);
    std::vector<uint8_t> result(metaSize);
    memcpy(result.data(), meta, metaSize);
    free_camera_metadata(meta);

    return result;
}

}  // namespace virtualcamera
