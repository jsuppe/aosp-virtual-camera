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

/**
 * Build camera characteristics metadata blob.
 *
 * Contains all static camera properties: supported resolutions,
 * hardware level, capabilities, zoom range, FPS ranges, etc.
 *
 * @return Raw camera_metadata_t bytes
 */
std::vector<uint8_t> buildCameraCharacteristics();

/**
 * Build default request settings metadata blob.
 *
 * Minimal settings: control mode auto, AE on, AWB auto, zoom 1.0x.
 * Same settings used for all request templates.
 *
 * @return Raw camera_metadata_t bytes
 */
std::vector<uint8_t> buildDefaultRequestSettings();

/**
 * Build per-frame result metadata blob.
 *
 * Contains sensor timestamp, zoom ratio, and crop region.
 *
 * @param timestamp Sensor timestamp in nanoseconds
 * @return Raw camera_metadata_t bytes
 */
std::vector<uint8_t> buildResultMetadata(int64_t timestamp);

}  // namespace virtualcamera
