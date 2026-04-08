/*
 * FrameFiller - Buffer filling utilities for virtual camera pipeline
 */

#define LOG_TAG "VCamFrameFiller"

#include "FrameFiller.h"
#include "HandleImporterCompat.h"
#include "VirtualCameraFrameSource.h"
#include "VirtualCameraFrameSourceV2.h"

#include <android/hardware_buffer.h>
#include <log/log.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace virtualcamera {

bool FrameFiller::fillYuvBufferFromRenderer(
        HandleImporter& importer,
        buffer_handle_t handle,
        int width, int height,
        int frameNumber,
        VirtualCameraFrameSource* frameSource) {

    if (handle == nullptr) {
        return false;
    }

    // Lock buffer for CPU write access (YCbCr format)
    auto ycbcr = lockYCbCrCompat(importer, handle,
            0x00000030U, width, height);  // GRALLOC_USAGE_SW_WRITE_OFTEN

    if (ycbcr.y == nullptr) {
        ALOGE("fillYuvBufferFromRenderer: Failed to lock buffer for CPU access");
        return false;
    }

    uint8_t* yPlane = static_cast<uint8_t*>(ycbcr.y);
    uint8_t* cbPlane = static_cast<uint8_t*>(ycbcr.cb);
    uint8_t* crPlane = static_cast<uint8_t*>(ycbcr.cr);
    int yStride = ycbcr.ystride;
    int cStride = ycbcr.cstride;
    int chromaStep = ycbcr.chroma_step;

    // Try to get frame from renderer
    bool usedRendererFrame = false;
    if (frameSource && frameSource->isRendererActive()) {
        uint32_t srcFormat = 0;
        frameSource->getFrameInfo(nullptr, nullptr, &srcFormat);

        // Allocate temp buffer for frame data from renderer
        size_t bufSize = (srcFormat == FORMAT_YUV_420)
            ? (width * height * 3 / 2)  // YUV420: Y + UV/2
            : (width * height * 4);     // RGBA
        std::vector<uint8_t> frameData(bufSize);

        uint32_t srcWidth, srcHeight;
        uint64_t timestamp;

        if (frameSource->acquireFrame(frameData.data(), bufSize,
                                       &srcWidth, &srcHeight, &timestamp)) {
            // Check dimensions match
            if (srcWidth == static_cast<uint32_t>(width) &&
                srcHeight == static_cast<uint32_t>(height)) {

                if (srcFormat == FORMAT_YUV_420) {
                    // YUV passthrough -- direct plane copy, no conversion!
                    const uint8_t* srcY = frameData.data();
                    const uint8_t* srcUV = srcY + width * height;

                    // Copy Y plane
                    for (int y = 0; y < height; y++) {
                        memcpy(yPlane + y * yStride, srcY + y * width, width);
                    }

                    // Copy UV (source is interleaved NV12: CbCr pairs)
                    int chromaHeight = height / 2;
                    int chromaWidth = width / 2;
                    if (chromaStep == 2) {
                        // Dest is also interleaved -- direct row copy
                        for (int cy = 0; cy < chromaHeight; cy++) {
                            memcpy(cbPlane + cy * cStride,
                                   srcUV + cy * width, width);
                        }
                    } else {
                        // Dest is planar (I420) -- deinterleave
                        for (int cy = 0; cy < chromaHeight; cy++) {
                            for (int cx = 0; cx < chromaWidth; cx++) {
                                cbPlane[cy * cStride + cx] = srcUV[cy * width + cx * 2];
                                crPlane[cy * cStride + cx] = srcUV[cy * width + cx * 2 + 1];
                            }
                        }
                    }

                    if (frameNumber % 100 == 0) {
                        ALOGI("YUV passthrough frame %lu (zero conversion)",
                              (unsigned long)timestamp);
                    }
                } else {
                    // RGBA -> YUV conversion (fallback)
                    const uint8_t* rgba = frameData.data();

                    for (int y = 0; y < height; y++) {
                        for (int x = 0; x < width; x++) {
                            int rgbaIdx = (y * width + x) * 4;
                            uint8_t r = rgba[rgbaIdx + 0];
                            uint8_t g = rgba[rgbaIdx + 1];
                            uint8_t b = rgba[rgbaIdx + 2];

                            uint8_t yVal, cbVal, crVal;
                            rgbaToYuv(r, g, b, &yVal, &cbVal, &crVal);

                            yPlane[y * yStride + x] = yVal;
                        }
                    }

                    int chromaHeight = height / 2;
                    int chromaWidth = width / 2;
                    for (int cy = 0; cy < chromaHeight; cy++) {
                        for (int cx = 0; cx < chromaWidth; cx++) {
                            int sx = cx * 2;
                            int sy = cy * 2;
                            int rgbaIdx = (sy * width + sx) * 4;
                            uint8_t r = rgba[rgbaIdx + 0];
                            uint8_t g = rgba[rgbaIdx + 1];
                            uint8_t b = rgba[rgbaIdx + 2];

                            uint8_t yVal, cbVal, crVal;
                            rgbaToYuv(r, g, b, &yVal, &cbVal, &crVal);

                            if (chromaStep == 2) {
                                cbPlane[cy * cStride + cx * 2] = cbVal;
                                crPlane[cy * cStride + cx * 2] = crVal;
                            } else {
                                cbPlane[cy * cStride + cx] = cbVal;
                                crPlane[cy * cStride + cx] = crVal;
                            }
                        }
                    }

                    if (frameNumber % 100 == 0) {
                        ALOGI("Using renderer frame (RGBA->YUV)");
                    }
                }

                usedRendererFrame = true;
            } else {
                ALOGW("Renderer frame size mismatch: %ux%u vs %dx%d",
                      srcWidth, srcHeight, width, height);
            }
        }
    }

    // If no renderer frame, fill with black
    if (!usedRendererFrame) {
        // Black in YUV: Y=16, Cb=128, Cr=128
        memset(yPlane, 16, yStride * height);

        int chromaHeight = height / 2;
        for (int y = 0; y < chromaHeight; y++) {
            if (chromaStep == 2) {
                // Interleaved
                for (int x = 0; x < width; x++) {
                    cbPlane[y * cStride + x] = 128;
                }
            } else {
                // Planar
                memset(cbPlane + y * cStride, 128, width / 2);
                memset(crPlane + y * cStride, 128, width / 2);
            }
        }
    }

    // Unlock buffer
    importer.unlock(handle);

    return usedRendererFrame;
}

bool FrameFiller::fillBufferFromV2(
        HandleImporter& importer,
        buffer_handle_t handle,
        int width, int height,
        VirtualCameraFrameSourceV2* frameSourceV2) {

    auto* halIface = frameSourceV2->getHalInterface();
    if (!halIface) return false;

    // Acquire latest frame from the zero-copy ring
    auto frame = halIface->acquireLatestFrame(/* timeoutMs= */ 16);
    if (!frame.valid()) return false;

    // Describe the source buffer to determine its format
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(frame.buffer, &desc);

    bool isSourceYuv =
        (desc.format == AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420);
    bool isSourceRgba =
        (desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
         desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);

    bool success;
    if (isSourceYuv) {
        // Renderer provided YUV -- direct plane copy, no conversion
        success = fillBufferFromV2Yuv(importer, handle, width, height, frame);
    } else if (isSourceRgba) {
        // Renderer provided RGBA -- convert to YUV
        success = fillBufferFromV2Rgba(importer, handle, width, height, frame);
    } else {
        ALOGE("fillBufferFromV2: unsupported source format %u", desc.format);
        success = false;
    }

    halIface->releaseFrame(frame);

    return success;
}

bool FrameFiller::fillBufferFromV2Yuv(
        HandleImporter& importer,
        buffer_handle_t handle,
        int width, int height,
        const HalInterface::AcquiredFrame& frame) {

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(frame.buffer, &desc);

    // Lock source as YCbCr
    AHardwareBuffer_Planes srcPlanes;
    int lockResult = AHardwareBuffer_lockPlanes(
        frame.buffer,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
        -1, nullptr, &srcPlanes);
    if (lockResult != 0) {
        ALOGE("fillBufferFromV2Yuv: Failed to lock source AHB");
        return false;
    }

    // Lock destination gralloc buffer as YCbCr
    auto dstYcbcr = lockYCbCrCompat(importer, handle,
        0x00000030U, width, height);
    if (dstYcbcr.y == nullptr) {
        ALOGE("fillBufferFromV2Yuv: Failed to lock output buffer");
        AHardwareBuffer_unlock(frame.buffer, nullptr);
        return false;
    }

    int copyWidth = std::min(width, static_cast<int>(desc.width));
    int copyHeight = std::min(height, static_cast<int>(desc.height));

    // Direct Y plane copy (row by row for stride compatibility)
    const uint8_t* srcY = static_cast<const uint8_t*>(srcPlanes.planes[0].data);
    uint8_t* dstY = static_cast<uint8_t*>(dstYcbcr.y);
    uint32_t srcYStride = srcPlanes.planes[0].rowStride;
    int dstYStride = dstYcbcr.ystride;

    for (int y = 0; y < copyHeight; y++) {
        memcpy(dstY + y * dstYStride, srcY + y * srcYStride, copyWidth);
    }

    // Direct UV plane copy
    // Source plane layout from AHardwareBuffer_lockPlanes:
    //   planes[1] = Cb, planes[2] = Cr
    const uint8_t* srcCb = static_cast<const uint8_t*>(srcPlanes.planes[1].data);
    const uint8_t* srcCr = static_cast<const uint8_t*>(srcPlanes.planes[2].data);
    uint32_t srcCStride = srcPlanes.planes[1].rowStride;
    uint32_t srcCPixelStride = srcPlanes.planes[1].pixelStride;

    uint8_t* dstCb = static_cast<uint8_t*>(dstYcbcr.cb);
    uint8_t* dstCr = static_cast<uint8_t*>(dstYcbcr.cr);
    int dstCStride = dstYcbcr.cstride;
    int dstChromaStep = dstYcbcr.chroma_step;

    int chromaW = copyWidth / 2;
    int chromaH = copyHeight / 2;

    if (srcCPixelStride == static_cast<uint32_t>(dstChromaStep) &&
        srcCStride == static_cast<uint32_t>(dstCStride)) {
        // Same layout -- bulk copy chroma planes
        size_t chromaRowBytes = chromaW * dstChromaStep;
        for (int y = 0; y < chromaH; y++) {
            memcpy(dstCb + y * dstCStride, srcCb + y * srcCStride, chromaRowBytes);
            if (dstChromaStep == 1) {
                // Planar -- Cr is a separate plane
                memcpy(dstCr + y * dstCStride, srcCr + y * srcCStride, chromaRowBytes);
            }
        }
    } else {
        // Different layouts -- pixel-by-pixel copy
        for (int cy = 0; cy < chromaH; cy++) {
            for (int cx = 0; cx < chromaW; cx++) {
                uint8_t cb = srcCb[cy * srcCStride + cx * srcCPixelStride];
                uint8_t cr = srcCr[cy * srcCStride + cx * srcCPixelStride];

                if (dstChromaStep == 2) {
                    dstCb[cy * dstCStride + cx * 2] = cb;
                    dstCr[cy * dstCStride + cx * 2] = cr;
                } else {
                    dstCb[cy * dstCStride + cx] = cb;
                    dstCr[cy * dstCStride + cx] = cr;
                }
            }
        }
    }

    importer.unlock(handle);
    AHardwareBuffer_unlock(frame.buffer, nullptr);
    return true;
}

bool FrameFiller::fillBufferFromV2Rgba(
        HandleImporter& importer,
        buffer_handle_t handle,
        int width, int height,
        const HalInterface::AcquiredFrame& frame) {

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(frame.buffer, &desc);

    // Lock source RGBA buffer
    void* srcPtr = nullptr;
    int lockResult = AHardwareBuffer_lock(
        frame.buffer,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
        -1, nullptr, &srcPtr);
    if (lockResult != 0 || !srcPtr) {
        ALOGE("fillBufferFromV2Rgba: Failed to lock source AHB");
        return false;
    }

    // Lock destination gralloc buffer as YCbCr
    auto ycbcr = lockYCbCrCompat(importer, handle,
        0x00000030U, width, height);
    if (ycbcr.y == nullptr) {
        ALOGE("fillBufferFromV2Rgba: Failed to lock output buffer");
        AHardwareBuffer_unlock(frame.buffer, nullptr);
        return false;
    }

    const uint8_t* rgba = static_cast<const uint8_t*>(srcPtr);
    uint32_t srcStride = desc.stride * 4;  // RGBA stride in bytes

    uint8_t* yPlane = static_cast<uint8_t*>(ycbcr.y);
    uint8_t* cbPlane = static_cast<uint8_t*>(ycbcr.cb);
    uint8_t* crPlane = static_cast<uint8_t*>(ycbcr.cr);
    int yStride = ycbcr.ystride;
    int cStride = ycbcr.cstride;
    int chromaStep = ycbcr.chroma_step;

    int copyWidth = std::min(width, static_cast<int>(desc.width));
    int copyHeight = std::min(height, static_cast<int>(desc.height));

    // BT.601 RGBA -> YUV conversion
    for (int y = 0; y < copyHeight; y++) {
        const uint8_t* row = rgba + y * srcStride;
        uint8_t* yRow = yPlane + y * yStride;
        for (int x = 0; x < copyWidth; x++) {
            uint8_t r = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t b = row[x * 4 + 2];
            yRow[x] = static_cast<uint8_t>(
                std::clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 0, 255));
        }
    }

    int chromaH = copyHeight / 2;
    int chromaW = copyWidth / 2;
    for (int cy = 0; cy < chromaH; cy++) {
        const uint8_t* row = rgba + (cy * 2) * srcStride;
        for (int cx = 0; cx < chromaW; cx++) {
            uint8_t r = row[cx * 2 * 4 + 0];
            uint8_t g = row[cx * 2 * 4 + 1];
            uint8_t b = row[cx * 2 * 4 + 2];

            uint8_t cbVal = static_cast<uint8_t>(
                std::clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 0, 255));
            uint8_t crVal = static_cast<uint8_t>(
                std::clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 0, 255));

            if (chromaStep == 2) {
                cbPlane[cy * cStride + cx * 2] = cbVal;
                crPlane[cy * cStride + cx * 2] = crVal;
            } else {
                cbPlane[cy * cStride + cx] = cbVal;
                crPlane[cy * cStride + cx] = crVal;
            }
        }
    }

    importer.unlock(handle);
    AHardwareBuffer_unlock(frame.buffer, nullptr);
    return true;
}

}  // namespace virtualcamera
