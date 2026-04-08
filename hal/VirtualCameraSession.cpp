/*
 * VirtualCameraSession - Implementation with buffer filling via HandleImporter
 */

#define LOG_TAG "VirtualCameraSession"

#include "VirtualCameraSession.h"

#include <log/log.h>
#include <system/camera_metadata.h>
#include <aidl/android/hardware/camera/device/ErrorCode.h>
#include <aidl/android/hardware/camera/device/ErrorMsg.h>
#include <aidl/android/hardware/camera/device/BufferStatus.h>
#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidlcommonsupport/NativeHandle.h>

#include <android/hardware_buffer.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace aidl::android::hardware::camera::provider::implementation {

using ::aidl::android::hardware::camera::device::BufferStatus;
using ::aidl::android::hardware::camera::device::ErrorCode;
using ::aidl::android::hardware::camera::device::ErrorMsg;
using ::aidl::android::hardware::graphics::common::BufferUsage;
using ::android::hardware::camera::common::V1_0::helper::HandleImporter;

// Static HandleImporter instance (shared across all sessions)
HandleImporter VirtualCameraSession::sHandleImporter;

VirtualCameraSession::VirtualCameraSession(
        const std::shared_ptr<ICameraDeviceCallback>& callback,
        std::shared_ptr<VirtualCameraFrameSource> frameSource,
        std::shared_ptr<VirtualCameraFrameSourceV2> frameSourceV2)
    : mCallback(callback),
      mFrameSource(frameSource),
      mFrameSourceV2(frameSourceV2) {
    ALOGI("VirtualCameraSession created (v1 + v2 frame sources)");
}

VirtualCameraSession::~VirtualCameraSession() {
    ALOGI("VirtualCameraSession destroyed, clearing %zu cached buffers", mBufferCache.size());
    
    // Free all cached buffer handles
    for (auto& pair : mBufferCache) {
        if (pair.second != nullptr) {
            sHandleImporter.freeBuffer(pair.second);
        }
    }
    mBufferCache.clear();
    
    close();
}

ndk::ScopedAStatus VirtualCameraSession::close() {
    ALOGI("Session close requested");
    mClosed = true;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::configureStreams(
        const StreamConfiguration& requestedConfiguration,
        std::vector<HalStream>* halStreams) {
    
    std::lock_guard<std::mutex> lock(mLock);
    
    if (mClosed) {
        return ndk::ScopedAStatus::fromServiceSpecificError(
                static_cast<int32_t>(CameraStatus::INTERNAL_ERROR));
    }
    
    // Clear old streams and buffer cache
    mStreams.clear();
    for (auto& pair : mBufferCache) {
        if (pair.second != nullptr) {
            sHandleImporter.freeBuffer(pair.second);
        }
    }
    mBufferCache.clear();
    halStreams->clear();
    
    ALOGI("Configuring %zu streams", requestedConfiguration.streams.size());
    
    for (const auto& stream : requestedConfiguration.streams) {
        ALOGI("Stream %d: %dx%d format=%d",
              stream.id, stream.width, stream.height,
              static_cast<int>(stream.format));
        
        // Store stream config
        mStreams[stream.id] = stream;
        
        // Return HAL stream configuration
        HalStream halStream;
        halStream.id = stream.id;
        halStream.overrideFormat = stream.format;
        halStream.producerUsage = static_cast<BufferUsage>(
                static_cast<int64_t>(BufferUsage::CPU_WRITE_OFTEN) |
                static_cast<int64_t>(BufferUsage::CAMERA_OUTPUT));
        halStream.consumerUsage = static_cast<BufferUsage>(0);
        halStream.maxBuffers = 4;
        halStream.overrideDataSpace = stream.dataSpace;
        halStream.physicalCameraId = "";
        halStream.supportOffline = false;
        
        halStreams->push_back(std::move(halStream));
    }
    
    // Publish the primary stream's format to the v2 renderer so it
    // can choose to provide buffers in the matching format.
    // We pick the first (largest) stream as the negotiation target.
    if (mFrameSourceV2 && !requestedConfiguration.streams.empty()) {
        const auto& primary = requestedConfiguration.streams[0];
        // Map HAL pixel format to AHARDWAREBUFFER_FORMAT
        int32_t ahbFormat;
        switch (static_cast<int>(primary.format)) {
            case HAL_PIXEL_FORMAT_YCbCr_420_888:
                ahbFormat = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;
                break;
            case HAL_PIXEL_FORMAT_RGBA_8888:
                ahbFormat = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
                break;
            case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
                // IMPLEMENTATION_DEFINED is typically YUV or RGBA depending
                // on consumer. Default to RGBA for flexibility.
                ahbFormat = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
                break;
            default:
                ahbFormat = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
                break;
        }

        uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
                       | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
                       | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;

        mFrameSourceV2->requestFormat(ahbFormat,
                                      primary.width, primary.height,
                                      usage);

        ALOGI("V2 format negotiation: requested AHB format=%d %dx%d",
              ahbFormat, primary.width, primary.height);
    }

    // Also publish format request to v1 renderer via shared memory header
    if (mFrameSource && !requestedConfiguration.streams.empty()) {
        const auto& primary = requestedConfiguration.streams[0];
        // Request YUV from v1 renderer to avoid RGBA→YUV conversion
        mFrameSource->requestFormat(FORMAT_YUV_420,
                                    primary.width, primary.height);
    }

    ALOGI("Streams configured successfully");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::constructDefaultRequestSettings(
        RequestTemplate type,
        CameraMetadata* metadata) {
    
    (void)type;  // All templates return the same minimal settings
    
    camera_metadata_t* meta = allocate_camera_metadata(10, 200);
    
    uint8_t controlMode = ANDROID_CONTROL_MODE_AUTO;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_MODE, &controlMode, 1);
    
    uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AE_MODE, &aeMode, 1);
    
    uint8_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
    
    float zoomRatio = 1.0f;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_ZOOM_RATIO, &zoomRatio, 1);
    
    size_t metaSize = get_camera_metadata_size(meta);
    metadata->metadata.resize(metaSize);
    memcpy(metadata->metadata.data(), meta, metaSize);
    free_camera_metadata(meta);
    
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::flush() {
    ALOGI("Flush requested");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::getCaptureRequestMetadataQueue(
        MQDescriptor<int8_t, SynchronizedReadWrite>* /*queue*/) {
    // FMQ not used in this implementation
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::getCaptureResultMetadataQueue(
        MQDescriptor<int8_t, SynchronizedReadWrite>* /*queue*/) {
    // FMQ not used in this implementation
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::isReconfigurationRequired(
        const CameraMetadata& /*oldSessionParams*/,
        const CameraMetadata& /*newSessionParams*/,
        bool* required) {
    *required = false;
    return ndk::ScopedAStatus::ok();
}

buffer_handle_t VirtualCameraSession::importBuffer(const StreamBuffer& buffer) {
    BufferKey key(buffer.streamId, buffer.bufferId);
    
    // Check if already cached
    auto it = mBufferCache.find(key);
    if (it != mBufferCache.end()) {
        return it->second;
    }
    
    // Buffer not cached - need to import it
    if (buffer.buffer.fds.empty()) {
        ALOGE("importBuffer: No handle provided and buffer not in cache (streamId=%d, bufferId=%lu)",
              buffer.streamId, (unsigned long)buffer.bufferId);
        return nullptr;
    }
    
    // Convert AIDL NativeHandle to native_handle_t, then to buffer_handle_t
    native_handle_t* nativeHandle = ::android::makeFromAidl(buffer.buffer);
    if (nativeHandle == nullptr || nativeHandle->numFds < 1) {
        ALOGE("importBuffer: Invalid handle from AIDL (numFds=%d)",
              nativeHandle ? nativeHandle->numFds : -1);
        if (nativeHandle) {
            native_handle_close(nativeHandle);
            native_handle_delete(nativeHandle);
        }
        return nullptr;
    }
    
    // Import the buffer using HandleImporter
    buffer_handle_t bufHandle = nativeHandle;
    if (!sHandleImporter.importBuffer(bufHandle)) {
        ALOGE("importBuffer: HandleImporter.importBuffer failed");
        native_handle_close(nativeHandle);
        native_handle_delete(nativeHandle);
        return nullptr;
    }
    
    // Cache it
    mBufferCache[key] = bufHandle;
    
    if (mBufferCache.size() <= 8) {  // Only log first few
        ALOGI("importBuffer: Cached buffer (streamId=%d, bufferId=%lu, numFds=%d)",
              buffer.streamId, (unsigned long)buffer.bufferId, nativeHandle->numFds);
    }
    
    return bufHandle;
}

void VirtualCameraSession::removeBuffersFromCache(const std::vector<BufferCache>& cachesToRemove) {
    for (const auto& cache : cachesToRemove) {
        BufferKey key(cache.streamId, cache.bufferId);
        auto it = mBufferCache.find(key);
        if (it != mBufferCache.end()) {
            if (it->second != nullptr) {
                sHandleImporter.freeBuffer(it->second);
            }
            mBufferCache.erase(it);
        }
    }
}

ndk::ScopedAStatus VirtualCameraSession::processCaptureRequest(
        const std::vector<CaptureRequest>& requests,
        const std::vector<BufferCache>& cachesToRemove,
        int32_t* numRequestsProcessed) {
    
    if (mClosed) {
        *numRequestsProcessed = 0;
        return ndk::ScopedAStatus::fromServiceSpecificError(
                static_cast<int32_t>(CameraStatus::INTERNAL_ERROR));
    }
    
    if (!cachesToRemove.empty()) {
        removeBuffersFromCache(cachesToRemove);
    }
    
    *numRequestsProcessed = 0;
    
    for (const auto& request : requests) {
        CameraStatus status = processSingleRequest(request);
        if (status != CameraStatus::OK) {
            ALOGE("Failed to process request %d", request.frameNumber);
            break;
        }
        (*numRequestsProcessed)++;
    }
    
    return ndk::ScopedAStatus::ok();
}

CameraStatus VirtualCameraSession::processSingleRequest(const CaptureRequest& request) {
    // Get timestamp
    auto now = std::chrono::steady_clock::now();
    int64_t timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    
    // Send shutter notification
    {
        std::vector<NotifyMsg> msgs(1);
        msgs[0].set<NotifyMsg::Tag::shutter>(ShutterMsg{
            .frameNumber = request.frameNumber,
            .timestamp = timestamp,
            .readoutTimestamp = timestamp
        });
        mCallback->notify(msgs);
    }
    
    // Process each output buffer
    std::vector<StreamBuffer> outputBuffers;
    outputBuffers.reserve(request.outputBuffers.size());
    
    for (size_t i = 0; i < request.outputBuffers.size(); i++) {
        const auto& inBuffer = request.outputBuffers[i];

        // Import/cache the buffer
        buffer_handle_t handle = importBuffer(inBuffer);

        // Get stream info for dimensions
        auto streamIt = mStreams.find(inBuffer.streamId);
        if (streamIt != mStreams.end() && handle != nullptr) {
            int width = streamIt->second.width;
            int height = streamIt->second.height;

            // Try v2 zero-copy first, fall back to v1
            bool filled = false;
            if (mFrameSourceV2 && mFrameSourceV2->isActive()) {
                filled = fillBufferFromV2(handle, width, height);
            }
            if (!filled) {
                fillYuvBuffer(handle, width, height, mFrameCounter.load());
            }
        }

        StreamBuffer outBuffer;
        outBuffer.streamId = inBuffer.streamId;
        outBuffer.bufferId = inBuffer.bufferId;
        outBuffer.status = BufferStatus::OK;

        outputBuffers.push_back(std::move(outBuffer));
        mFrameCounter++;
    }
    
    // Log periodically
    if (mFrameCounter % 100 == 0) {
        ALOGI("Processed %d frames", mFrameCounter.load());
    }
    
    // Create result metadata
    camera_metadata_t* meta = allocate_camera_metadata(10, 200);
    
    int64_t ts = timestamp;
    add_camera_metadata_entry(meta, ANDROID_SENSOR_TIMESTAMP, &ts, 1);
    
    float zoomRatio = 1.0f;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_ZOOM_RATIO, &zoomRatio, 1);
    
    int32_t cropRegion[] = {0, 0, 3840, 2160};
    add_camera_metadata_entry(meta, ANDROID_SCALER_CROP_REGION, cropRegion, 4);
    
    // Build capture result
    CaptureResult captureResult;
    captureResult.frameNumber = request.frameNumber;
    captureResult.fmqResultSize = 0;
    captureResult.outputBuffers = std::move(outputBuffers);
    captureResult.inputBuffer.streamId = -1;
    captureResult.partialResult = 1;
    captureResult.physicalCameraMetadata = {};
    
    size_t metaSize = get_camera_metadata_size(meta);
    captureResult.result.metadata.resize(metaSize);
    memcpy(captureResult.result.metadata.data(), meta, metaSize);
    free_camera_metadata(meta);
    
    std::vector<CaptureResult> results;
    results.push_back(std::move(captureResult));
    mCallback->processCaptureResult(results);
    
    return CameraStatus::OK;
}

bool VirtualCameraSession::fillBufferFromV2(
        buffer_handle_t handle, int width, int height) {

    auto* halIface = mFrameSourceV2->getHalInterface();
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
        // Renderer provided YUV — direct plane copy, no conversion
        success = fillBufferFromV2Yuv(handle, width, height, frame);
    } else if (isSourceRgba) {
        // Renderer provided RGBA — convert to YUV
        success = fillBufferFromV2Rgba(handle, width, height, frame);
    } else {
        ALOGE("fillBufferFromV2: unsupported source format %u", desc.format);
        success = false;
    }

    halIface->releaseFrame(frame);

    if (success && mFrameCounter % 100 == 0) {
        ALOGI("V2 frame %d (%s, dropped=%u)",
              mFrameCounter.load(),
              isSourceYuv ? "YUV passthrough" : "RGBA→YUV",
              halIface->getDroppedFrameCount());
    }

    return success;
}

bool VirtualCameraSession::fillBufferFromV2Yuv(
        buffer_handle_t handle, int width, int height,
        const virtualcamera::HalInterface::AcquiredFrame& frame) {

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
    ::android::Rect region(0, 0, width, height);
    android_ycbcr dstYcbcr = sHandleImporter.lockYCbCr(
        handle, 0x00000030U, region);
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
        // Same layout — bulk copy chroma planes
        size_t chromaRowBytes = chromaW * dstChromaStep;
        for (int y = 0; y < chromaH; y++) {
            memcpy(dstCb + y * dstCStride, srcCb + y * srcCStride, chromaRowBytes);
            if (dstChromaStep == 1) {
                // Planar — Cr is a separate plane
                memcpy(dstCr + y * dstCStride, srcCr + y * srcCStride, chromaRowBytes);
            }
        }
    } else {
        // Different layouts — pixel-by-pixel copy
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

    sHandleImporter.unlock(handle);
    AHardwareBuffer_unlock(frame.buffer, nullptr);
    return true;
}

bool VirtualCameraSession::fillBufferFromV2Rgba(
        buffer_handle_t handle, int width, int height,
        const virtualcamera::HalInterface::AcquiredFrame& frame) {

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
    ::android::Rect region(0, 0, width, height);
    android_ycbcr ycbcr = sHandleImporter.lockYCbCr(
        handle, 0x00000030U, region);
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

    // BT.601 RGBA → YUV conversion
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

    sHandleImporter.unlock(handle);
    AHardwareBuffer_unlock(frame.buffer, nullptr);
    return true;
}

// Convert RGBA pixel to YUV (BT.601)
static inline void rgbaToYuv(uint8_t r, uint8_t g, uint8_t b,
                             uint8_t* y, uint8_t* cb, uint8_t* cr) {
    // BT.601 conversion
    int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    int cbVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    int crVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
    
    *y = static_cast<uint8_t>(std::clamp(yVal, 0, 255));
    *cb = static_cast<uint8_t>(std::clamp(cbVal, 0, 255));
    *cr = static_cast<uint8_t>(std::clamp(crVal, 0, 255));
}

void VirtualCameraSession::fillYuvBuffer(
        buffer_handle_t handle, int width, int height, int frameNumber) {
    
    if (handle == nullptr) {
        return;
    }
    
    // Lock buffer for CPU write access (YCbCr format)
    ::android::Rect region(0, 0, width, height);
    android_ycbcr ycbcr = sHandleImporter.lockYCbCr(
            handle,
            0x00000030U,  // GRALLOC_USAGE_SW_WRITE_OFTEN
            region);
    
    if (ycbcr.y == nullptr) {
        ALOGE("fillYuvBuffer: Failed to lock buffer for CPU access");
        return;
    }
    
    uint8_t* yPlane = static_cast<uint8_t*>(ycbcr.y);
    uint8_t* cbPlane = static_cast<uint8_t*>(ycbcr.cb);
    uint8_t* crPlane = static_cast<uint8_t*>(ycbcr.cr);
    int yStride = ycbcr.ystride;
    int cStride = ycbcr.cstride;
    int chromaStep = ycbcr.chroma_step;
    
    // Try to get frame from renderer first
    bool usedRendererFrame = false;
    if (mFrameSource && mFrameSource->isRendererActive()) {
        uint32_t srcFormat = 0;
        mFrameSource->getFrameInfo(nullptr, nullptr, &srcFormat);

        // Allocate temp buffer for frame data from renderer
        size_t bufSize = (srcFormat == FORMAT_YUV_420)
            ? (width * height * 3 / 2)  // YUV420: Y + UV/2
            : (width * height * 4);     // RGBA
        std::vector<uint8_t> frameData(bufSize);

        uint32_t srcWidth, srcHeight;
        uint64_t timestamp;

        if (mFrameSource->acquireFrame(frameData.data(), bufSize,
                                       &srcWidth, &srcHeight, &timestamp)) {
            // Check dimensions match
            if (srcWidth == static_cast<uint32_t>(width) &&
                srcHeight == static_cast<uint32_t>(height)) {

                if (srcFormat == FORMAT_YUV_420) {
                    // YUV passthrough — direct plane copy, no conversion!
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
                        // Dest is also interleaved — direct row copy
                        for (int cy = 0; cy < chromaHeight; cy++) {
                            memcpy(cbPlane + cy * cStride,
                                   srcUV + cy * width, width);
                        }
                    } else {
                        // Dest is planar (I420) — deinterleave
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
                    // RGBA → YUV conversion (fallback)
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
                        ALOGI("Using renderer frame %lu (RGBA→YUV)",
                              (unsigned long)timestamp);
                    }
                }

                usedRendererFrame = true;
                mLastFrameTimestamp = timestamp;
            } else {
                ALOGW("Renderer frame size mismatch: %ux%u vs %dx%d",
                      srcWidth, srcHeight, width, height);
            }
        }
    }
    
    // If no renderer frame, fill with black (no test pattern)
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
    sHandleImporter.unlock(handle);
}

ndk::ScopedAStatus VirtualCameraSession::signalStreamFlush(
        const std::vector<int32_t>& /*streamIds*/,
        int32_t /*streamConfigCounter*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::switchToOffline(
        const std::vector<int32_t>& /*streamsToKeep*/,
        CameraOfflineSessionInfo* /*offlineSessionInfo*/,
        std::shared_ptr<ICameraOfflineSession>* session) {
    *session = nullptr;
    return ndk::ScopedAStatus::fromServiceSpecificError(
            static_cast<int32_t>(CameraStatus::OPERATION_NOT_SUPPORTED));
}

ndk::ScopedAStatus VirtualCameraSession::repeatingRequestEnd(
        int32_t /*frameNumber*/,
        const std::vector<int32_t>& /*streamIds*/) {
    return ndk::ScopedAStatus::ok();
}

}  // namespace
