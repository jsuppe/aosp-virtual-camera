/*
 * JpegEncoder implementation (libjpeg-turbo).
 */

#define LOG_TAG "VCamJpeg"

#include "JpegEncoder.h"

#include <android/sync.h>
#include <log/log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

namespace virtualcamera {

namespace {

// hardware/camera3.h camera3_jpeg_blob: the framework reads this from the
// last sizeof(struct) bytes of the BLOB buffer. NOT packed: the framework's
// struct has natural alignment (sizeof 8, jpeg_size at offset 4); a packed
// 6-byte copy lands two bytes off and ImageReader reports the whole buffer.
struct JpegBlob {
    uint16_t jpeg_blob_id;
    uint32_t jpeg_size;
};
static_assert(sizeof(JpegBlob) == 8, "must match camera3_jpeg_blob layout");
constexpr uint16_t kJpegBlobId = 0x00FF;   // CameraBlobId::JPEG

}  // namespace

size_t JpegEncoder::encodeToBlob(AHardwareBuffer* src, int acquireFenceFd,
                                 int width, int height, int quality,
                                 void* dst, size_t capacity) {
    if (!src || !dst || width <= 0 || height <= 0 || capacity <= sizeof(JpegBlob)) return 0;

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(src, &desc);
    if (desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM &&
        desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM) {
        ALOGE("encodeToBlob: source format %u not RGBA", desc.format);
        return 0;
    }
    if (acquireFenceFd >= 0) sync_wait(acquireFenceFd, 100);

    void* srcPtr = nullptr;
    if (AHardwareBuffer_lock(src, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &srcPtr) != 0 ||
        !srcPtr) {
        ALOGE("encodeToBlob: source lock failed");
        return 0;
    }
    const uint8_t* srcBase = static_cast<const uint8_t*>(srcPtr);
    const size_t srcRowBytes = static_cast<size_t>(desc.stride) * 4;

    // Row buffer: the source row when sizes match, a nearest-neighbour
    // resample of it otherwise.
    const bool sameSize = (static_cast<int>(desc.width) == width &&
                           static_cast<int>(desc.height) == height);
    std::vector<uint8_t> row(sameSize ? 0 : static_cast<size_t>(width) * 4);

    unsigned char* outBuf = nullptr;
    unsigned long outSize = 0;
    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &outBuf, &outSize);   // libjpeg grows this buffer itself
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 4;
    cinfo.in_color_space = JCS_EXT_RGBA;        // libjpeg-turbo: no RGB pre-pass
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality > 0 && quality <= 100 ? quality : 90, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        const int y = cinfo.next_scanline;
        const uint8_t* line;
        if (sameSize) {
            line = srcBase + static_cast<size_t>(y) * srcRowBytes;
        } else {
            const int sy = static_cast<int>(static_cast<int64_t>(y) * desc.height / height);
            const uint8_t* srow = srcBase + static_cast<size_t>(sy) * srcRowBytes;
            for (int x = 0; x < width; x++) {
                const int sx = static_cast<int>(static_cast<int64_t>(x) * desc.width / width);
                memcpy(&row[static_cast<size_t>(x) * 4], srow + static_cast<size_t>(sx) * 4, 4);
            }
            line = row.data();
        }
        JSAMPROW rows[1] = { const_cast<JSAMPROW>(line) };
        jpeg_write_scanlines(&cinfo, rows, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    AHardwareBuffer_unlock(src, nullptr);

    size_t written = 0;
    if (outBuf && outSize > 0 && outSize + sizeof(JpegBlob) <= capacity) {
        memcpy(dst, outBuf, outSize);
        JpegBlob blob = { kJpegBlobId, static_cast<uint32_t>(outSize) };
        memcpy(static_cast<uint8_t*>(dst) + capacity - sizeof(JpegBlob), &blob, sizeof(blob));
        written = outSize;
    } else {
        ALOGE("encodeToBlob: JPEG %lu bytes does not fit in %zu", outSize, capacity);
    }
    free(outBuf);
    return written;
}

}  // namespace virtualcamera
