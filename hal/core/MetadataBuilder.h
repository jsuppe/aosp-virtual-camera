/*
 * MetadataBuilder - Camera metadata construction utilities
 *
 * Builds camera_metadata blobs for characteristics, default request
 * settings, and per-frame capture results. Returns raw byte vectors
 * that the AIDL adapter wraps into CameraMetadata.
 *
 * No AIDL dependencies -- uses only libcamera_metadata.
 */

#pragma once

#include <cstdint>
#include <vector>
#include "ProducerCapabilities.h"

namespace virtualcamera {

struct MetadataBuilder {
    /** Largest JPEG the HAL will produce (BLOB stream buffer size). */
    static constexpr int32_t kJpegMaxSize = 3840 * 2160 * 4 + 65536;   // 8 MB: 4K at any quality fits

    struct StreamDesc {
        int format;        // HAL_PIXEL_FORMAT_*
        int width, height;
        int64_t useCase;   // ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_*
        bool input;
        int rotation;      // StreamRotation: 0..3 valid
    };
    /** Advertised-config check shared by isStreamCombinationSupported and configureStreams. */
    static bool isStreamCombinationSupported(const std::vector<StreamDesc>& streams, const ProducerCapabilities* capabilities = nullptr);

    /** Build camera characteristics (supported resolutions, FPS, etc.) */
    static std::vector<uint8_t> buildCameraCharacteristics(const ProducerCapabilities* capabilities = nullptr);

    /** Build default request settings (AE auto, AWB auto, zoom 1.0x) */
    static std::vector<uint8_t> buildDefaultRequestSettings(const ProducerCapabilities* capabilities = nullptr);

    /** Build per-frame result metadata (timestamp, frame duration, zoom, crop) */
    static std::vector<uint8_t> buildResultMetadata(int64_t timestamp,
                                                    int64_t frameDurationNs = 33333333,
                                                    int64_t producerTimestampNs = 0, int width = 3840, int height = 2160);
};

}  // namespace virtualcamera
