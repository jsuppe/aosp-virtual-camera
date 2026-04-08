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

namespace virtualcamera {

struct MetadataBuilder {
    /** Build camera characteristics (supported resolutions, FPS, etc.) */
    static std::vector<uint8_t> buildCameraCharacteristics();

    /** Build default request settings (AE auto, AWB auto, zoom 1.0x) */
    static std::vector<uint8_t> buildDefaultRequestSettings();

    /** Build per-frame result metadata (timestamp, zoom, crop) */
    static std::vector<uint8_t> buildResultMetadata(int64_t timestamp);
};

}  // namespace virtualcamera
