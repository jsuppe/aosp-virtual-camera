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

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace aidl::android::hardware::camera::provider::implementation {

using ::aidl::android::hardware::camera::device::BufferStatus;
using ::aidl::android::hardware::camera::device::ErrorCode;
using ::aidl::android::hardware::camera::device::ErrorMsg;
using ::aidl::android::hardware::graphics::common::BufferUsage;
using ::android::hardware::camera::common::helper::HandleImporter;

// Static HandleImporter instance (shared across all sessions)
HandleImporter VirtualCameraSession::sHandleImporter;

VirtualCameraSession::VirtualCameraSession(
        const std::shared_ptr<ICameraDeviceCallback>& callback)
    : mCallback(callback),
      mFrameSource(std::make_unique<VirtualCameraFrameSource>()) {
    ALOGI("VirtualCameraSession created with FrameSource");
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
            
            // Fill buffer with test pattern
            fillYuvBuffer(handle, width, height, mFrameCounter.load());
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
    
    int32_t cropRegion[] = {0, 0, 1920, 1080};
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
        // Allocate temp buffer for RGBA data from renderer
        size_t rgbaSize = width * height * 4;
        std::vector<uint8_t> rgbaBuffer(rgbaSize);
        
        uint32_t srcWidth, srcHeight;
        uint64_t timestamp;
        
        if (mFrameSource->acquireFrame(rgbaBuffer.data(), rgbaSize,
                                       &srcWidth, &srcHeight, &timestamp)) {
            // Check dimensions match
            if (srcWidth == static_cast<uint32_t>(width) && 
                srcHeight == static_cast<uint32_t>(height)) {
                
                // Convert RGBA to YUV
                const uint8_t* rgba = rgbaBuffer.data();
                
                // Fill Y plane
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        int rgbaIdx = (y * width + x) * 4;
                        uint8_t r = rgba[rgbaIdx + 0];
                        uint8_t g = rgba[rgbaIdx + 1];
                        uint8_t b = rgba[rgbaIdx + 2];
                        // alpha at rgbaIdx + 3 ignored
                        
                        uint8_t yVal, cbVal, crVal;
                        rgbaToYuv(r, g, b, &yVal, &cbVal, &crVal);
                        
                        yPlane[y * yStride + x] = yVal;
                    }
                }
                
                // Fill UV planes (subsampled 2x2)
                int chromaHeight = height / 2;
                int chromaWidth = width / 2;
                
                for (int cy = 0; cy < chromaHeight; cy++) {
                    for (int cx = 0; cx < chromaWidth; cx++) {
                        // Sample center of 2x2 block
                        int sx = cx * 2;
                        int sy = cy * 2;
                        int rgbaIdx = (sy * width + sx) * 4;
                        
                        uint8_t r = rgba[rgbaIdx + 0];
                        uint8_t g = rgba[rgbaIdx + 1];
                        uint8_t b = rgba[rgbaIdx + 2];
                        
                        uint8_t yVal, cbVal, crVal;
                        rgbaToYuv(r, g, b, &yVal, &cbVal, &crVal);
                        
                        if (chromaStep == 2) {
                            // Interleaved (NV12/NV21)
                            cbPlane[cy * cStride + cx * 2] = cbVal;
                            crPlane[cy * cStride + cx * 2] = crVal;
                        } else {
                            // Planar (I420)
                            cbPlane[cy * cStride + cx] = cbVal;
                            crPlane[cy * cStride + cx] = crVal;
                        }
                    }
                }
                
                usedRendererFrame = true;
                mLastFrameTimestamp = timestamp;
                
                if (frameNumber % 100 == 0) {
                    ALOGI("Using renderer frame %lu", (unsigned long)timestamp);
                }
            } else {
                ALOGW("Renderer frame size mismatch: %ux%u vs %dx%d",
                      srcWidth, srcHeight, width, height);
            }
        }
    }
    
    // Fall back to test pattern if no renderer frame
    if (!usedRendererFrame) {
        // Generate animated color bars test pattern
        int barWidth = width / 8;
        int offset = (frameNumber * 4) % width;
        
        // Color bar YUV values (8 bars cycling)
        // White, Yellow, Cyan, Green, Magenta, Red, Blue, Black
        const uint8_t barY[] = {235, 210, 170, 145, 106, 81, 41, 16};
        const uint8_t barCb[] = {128, 16, 166, 54, 202, 90, 240, 128};
        const uint8_t barCr[] = {128, 146, 16, 34, 222, 240, 110, 128};
        
        // Fill Y plane
        for (int y = 0; y < height; y++) {
            uint8_t* row = yPlane + y * yStride;
            for (int x = 0; x < width; x++) {
                int barIndex = ((x + offset) / barWidth) % 8;
                row[x] = barY[barIndex];
            }
        }
        
        // Fill UV planes
        int chromaHeight = height / 2;
        int chromaWidth = width / 2;
        
        for (int y = 0; y < chromaHeight; y++) {
            for (int x = 0; x < chromaWidth; x++) {
                int barIndex = ((x * 2 + offset) / barWidth) % 8;
                
                if (chromaStep == 2) {
                    cbPlane[y * cStride + x * 2] = barCb[barIndex];
                    crPlane[y * cStride + x * 2] = barCr[barIndex];
                } else {
                    cbPlane[y * cStride + x] = barCb[barIndex];
                    crPlane[y * cStride + x] = barCr[barIndex];
                }
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
