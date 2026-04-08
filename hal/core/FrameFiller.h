/*
 * FrameFiller - Buffer filling utilities for virtual camera pipeline
 *
 * Extracts frame data from VirtualCameraFrameSource (v1) and
 * VirtualCameraFrameSourceV2 (v2) into gralloc buffer handles.
 *
 * Uses HandleImporter (version-independent camera common helper) and
 * native buffer_handle_t. No AIDL dependencies.
 */

#pragma once

#include <HandleImporter.h>
#include <cutils/native_handle.h>
#include <algorithm>
#include <cstdint>

#include "HalInterface.h"

namespace virtualcamera {

// Forward declarations
class VirtualCameraFrameSource;
class VirtualCameraFrameSourceV2;

// Convert RGBA pixel to YUV (BT.601)
static inline void rgbaToYuv(uint8_t r, uint8_t g, uint8_t b,
                             uint8_t* y, uint8_t* cb, uint8_t* cr) {
    int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    int cbVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    int crVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

    *y = static_cast<uint8_t>(std::clamp(yVal, 0, 255));
    *cb = static_cast<uint8_t>(std::clamp(cbVal, 0, 255));
    *cr = static_cast<uint8_t>(std::clamp(crVal, 0, 255));
}

using HandleImporter = ::android::hardware::camera::common::V1_0::helper::HandleImporter;

/**
 * Fill a YUV gralloc buffer from the v1 renderer (shared memory).
 *
 * Acquires the latest frame from the frame source, detects format
 * (YUV passthrough vs RGBA conversion), and writes into the locked
 * gralloc buffer.
 *
 * @param importer    HandleImporter for buffer locking/unlocking
 * @param handle      Destination gralloc buffer handle
 * @param width       Buffer width
 * @param height      Buffer height
 * @param frameNumber Current frame number (for periodic logging)
 * @param frameSource V1 frame source to acquire from
 * @return true if renderer frame was used, false if filled with black
 */
bool fillYuvBufferFromRenderer(
        HandleImporter& importer,
        buffer_handle_t handle,
        int width, int height,
        int frameNumber,
        VirtualCameraFrameSource* frameSource);

/**
 * Fill a gralloc buffer from the v2 zero-copy source.
 *
 * Acquires the latest AHardwareBuffer from the HalInterface,
 * dispatches to YUV passthrough or RGBA->YUV conversion based
 * on source format.
 *
 * @param importer      HandleImporter for buffer locking/unlocking
 * @param handle        Destination gralloc buffer handle
 * @param width         Buffer width
 * @param height        Buffer height
 * @param frameSourceV2 V2 frame source to acquire from
 * @return true if frame was filled successfully
 */
bool fillBufferFromV2(
        HandleImporter& importer,
        buffer_handle_t handle,
        int width, int height,
        VirtualCameraFrameSourceV2* frameSourceV2);

/**
 * Fill a YUV gralloc buffer from a v2 YUV AHardwareBuffer (direct plane copy).
 *
 * @param importer HandleImporter for buffer locking/unlocking
 * @param handle   Destination gralloc buffer handle
 * @param width    Buffer width
 * @param height   Buffer height
 * @param frame    Acquired frame from HalInterface
 * @return true on success
 */
bool fillBufferFromV2Yuv(
        HandleImporter& importer,
        buffer_handle_t handle,
        int width, int height,
        const HalInterface::AcquiredFrame& frame);

/**
 * Fill a YUV gralloc buffer from a v2 RGBA AHardwareBuffer (BT.601 conversion).
 *
 * @param importer HandleImporter for buffer locking/unlocking
 * @param handle   Destination gralloc buffer handle
 * @param width    Buffer width
 * @param height   Buffer height
 * @param frame    Acquired frame from HalInterface
 * @return true on success
 */
bool fillBufferFromV2Rgba(
        HandleImporter& importer,
        buffer_handle_t handle,
        int width, int height,
        const HalInterface::AcquiredFrame& frame);

}  // namespace virtualcamera
