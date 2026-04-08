/*
 * VirtualCameraSession - AIDL V1 Adapter Implementation
 *
 * Owns the buffer cache and AIDL type conversions. Delegates all frame
 * filling to core::fillYuvBufferFromRenderer / core::fillBufferFromV2
 * and metadata building to core::MetadataBuilder.
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

// Core frame filling
#include "FrameFiller.h"

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
        std::shared_ptr<virtualcamera::VirtualCameraFrameSource> frameSource,
        std::shared_ptr<virtualcamera::VirtualCameraFrameSourceV2> frameSourceV2)
    : mCallback(callback),
      mFrameSource(frameSource),
      mFrameSourceV2(frameSourceV2) {
    ALOGI("VirtualCameraSession created (AIDL V1 adapter, v1 + v2 frame sources)");
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

    // Also publish format request to v1 renderer
    if (mFrameSource && !requestedConfiguration.streams.empty()) {
        const auto& primary = requestedConfiguration.streams[0];
        mFrameSource->requestFormat(virtualcamera::FORMAT_YUV_420,
                                    primary.width, primary.height);
    }

    ALOGI("Streams configured successfully");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::constructDefaultRequestSettings(
        RequestTemplate type,
        CameraMetadata* metadata) {

    (void)type;  // All templates return the same minimal settings
    metadata->metadata = virtualcamera::MetadataBuilder::buildDefaultRequestSettings();
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

        // Import/cache the buffer (adapter owns this — touches AIDL StreamBuffer)
        buffer_handle_t handle = importBuffer(inBuffer);

        // Get stream info for dimensions
        auto streamIt = mStreams.find(inBuffer.streamId);
        if (streamIt != mStreams.end() && handle != nullptr) {
            int width = streamIt->second.width;
            int height = streamIt->second.height;

            // Delegate frame filling to core — try v2 zero-copy first, fall back to v1
            bool filled = false;
            if (mFrameSourceV2 && mFrameSourceV2->isActive()) {
                filled = virtualcamera::FrameFiller::fillBufferFromV2(
                    handle, width, height, mFrameSourceV2.get(),
                    sHandleImporter, mFrameCounter.load());
            }
            if (!filled) {
                virtualcamera::FrameFiller::fillYuvBufferFromRenderer(
                    handle, width, height, mFrameCounter.load(),
                    mFrameSource.get(), sHandleImporter,
                    mLastFrameTimestamp);
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

    // Build result metadata via core::MetadataBuilder
    CaptureResult captureResult;
    captureResult.frameNumber = request.frameNumber;
    captureResult.fmqResultSize = 0;
    captureResult.outputBuffers = std::move(outputBuffers);
    captureResult.inputBuffer.streamId = -1;
    captureResult.partialResult = 1;
    captureResult.physicalCameraMetadata = {};
    captureResult.result.metadata = virtualcamera::MetadataBuilder::buildResultMetadata(timestamp);

    std::vector<CaptureResult> results;
    results.push_back(std::move(captureResult));
    mCallback->processCaptureResult(results);

    return CameraStatus::OK;
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

}  // namespace aidl::android::hardware::camera::provider::implementation
