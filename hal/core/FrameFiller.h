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

#include "HandleImporterCompat.h"
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

struct FrameFiller {
    /** Fill YUV gralloc buffer from v1 renderer shared memory */
    static bool fillYuvBufferFromRenderer(
            HandleImporter& importer,
            buffer_handle_t handle,
            int width, int height,
            int frameNumber,
            VirtualCameraFrameSource* frameSource);

    /** Fill gralloc buffer from v2 zero-copy source (auto-detects YUV vs RGBA) */
    static bool fillBufferFromV2(
            HandleImporter& importer,
            buffer_handle_t handle,
            int width, int height,
            VirtualCameraFrameSourceV2* frameSourceV2);

    /** Direct YUV plane copy from v2 AHardwareBuffer */
    static bool fillBufferFromV2Yuv(
            HandleImporter& importer,
            buffer_handle_t handle,
            int width, int height,
            const HalInterface::AcquiredFrame& frame);

    /** RGBA→YUV conversion from v2 AHardwareBuffer */
    static bool fillBufferFromV2Rgba(
            HandleImporter& importer,
            buffer_handle_t handle,
            int width, int height,
            const HalInterface::AcquiredFrame& frame);
};

}  // namespace virtualcamera
