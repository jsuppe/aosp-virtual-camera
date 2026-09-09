/*
 * JpegEncoder - BLOB (JPEG) output for the virtual camera.
 *
 * Encodes the producer's RGBA AHardwareBuffer into a camera BLOB buffer:
 * JPEG bytes at the start, camera3_jpeg_blob trailer at the very end of the
 * buffer (that is how the framework finds the JPEG length). libjpeg-turbo
 * consumes RGBA rows directly (JCS_EXT_RGBA), so there is no color-space
 * pre-pass; a nearest-neighbour downscale covers BLOB streams smaller than
 * the producer frame. CPU path by design: stills are rare and stall.
 */
#pragma once

#include <android/hardware_buffer.h>

#include <cstddef>
#include <cstdint>

namespace virtualcamera {

struct JpegEncoder {
    /**
     * Encode src (RGBA) as a width x height JPEG into dst (capacity bytes,
     * the BLOB gralloc buffer, CPU-locked by the caller). Writes the trailer.
     * Returns the JPEG size, or 0 on failure. acquireFenceFd is waited on
     * (CPU) before the source is read; not consumed.
     */
    static size_t encodeToBlob(AHardwareBuffer* src, int acquireFenceFd,
                               int width, int height, int quality,
                               void* dst, size_t capacity);
};

}  // namespace virtualcamera
