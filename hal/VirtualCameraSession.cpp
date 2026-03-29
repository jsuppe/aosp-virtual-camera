/*
 * VirtualCameraSession - Minimal implementation with test pattern output
 */

#define LOG_TAG "VirtualCameraSession"

#include "VirtualCameraSession.h"

#include <log/log.h>
#include <system/camera_metadata.h>
#include <hardware/gralloc.h>
#include <aidl/android/hardware/camera/device/ErrorCode.h>
#include <aidl/android/hardware/camera/device/ErrorMsg.h>
#include <aidl/android/hardware/camera/device/BufferStatus.h>
#include <aidl/android/hardware/graphics/common/BufferUsage.h>

#include <chrono>

namespace aidl::android::hardware::camera::provider::implementation {

using ::aidl::android::hardware::camera::device::BufferStatus;
using ::aidl::android::hardware::camera::device::ErrorCode;
using ::aidl::android::hardware::camera::device::ErrorMsg;
using ::aidl::android::hardware::graphics::common::BufferUsage;

VirtualCameraSession::VirtualCameraSession(
        const std::shared_ptr<ICameraDeviceCallback>& callback)
    : mCallback(callback) {
    ALOGI("VirtualCameraSession created");
}

VirtualCameraSession::~VirtualCameraSession() {
    ALOGI("VirtualCameraSession destroyed");
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
    
    mStreams.clear();
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
        
        halStreams->push_back(halStream);
    }
    
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::constructDefaultRequestSettings(
        RequestTemplate type,
        CameraMetadata* metadata) {
    
    ALOGI("constructDefaultRequestSettings type=%d", static_cast<int>(type));
    
    // Create minimal metadata
    camera_metadata_t* meta = allocate_camera_metadata(10, 100);
    
    uint8_t controlMode = ANDROID_CONTROL_MODE_AUTO;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_MODE, &controlMode, 1);
    
    uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AE_MODE, &aeMode, 1);
    
    uint8_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
    
    // Copy to output
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
        MQDescriptor<int8_t, SynchronizedReadWrite>* queue) {
    // Return empty descriptor - metadata will be passed inline
    *queue = MQDescriptor<int8_t, SynchronizedReadWrite>();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::getCaptureResultMetadataQueue(
        MQDescriptor<int8_t, SynchronizedReadWrite>* queue) {
    // Return empty descriptor - metadata will be passed inline
    *queue = MQDescriptor<int8_t, SynchronizedReadWrite>();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::isReconfigurationRequired(
        const CameraMetadata& /*oldSessionParams*/,
        const CameraMetadata& /*newSessionParams*/,
        bool* required) {
    *required = false;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::processCaptureRequest(
        const std::vector<CaptureRequest>& requests,
        const std::vector<BufferCache>& /*cachesToRemove*/,
        int32_t* numRequestsProcessed) {
    
    if (mClosed) {
        *numRequestsProcessed = 0;
        return ndk::ScopedAStatus::fromServiceSpecificError(
                static_cast<int32_t>(CameraStatus::INTERNAL_ERROR));
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
    ALOGI("Processing frame %d with %zu output buffers",
          request.frameNumber, request.outputBuffers.size());
    
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
    
    // Process each output buffer - just mark as OK for now
    // TODO: Actually fill buffers with test pattern
    std::vector<StreamBuffer> outputBuffers;
    outputBuffers.reserve(request.outputBuffers.size());
    
    for (size_t i = 0; i < request.outputBuffers.size(); i++) {
        const auto& inBuffer = request.outputBuffers[i];
        
        StreamBuffer outBuffer;
        outBuffer.streamId = inBuffer.streamId;
        outBuffer.bufferId = inBuffer.bufferId;
        outBuffer.status = BufferStatus::OK;
        
        mFrameCounter++;
        outputBuffers.push_back(std::move(outBuffer));
    }
    
    // Send capture result
    std::vector<CaptureResult> results(1);
    results[0].frameNumber = request.frameNumber;
    results[0].outputBuffers = std::move(outputBuffers);
    results[0].partialResult = 1;
    
    // Create result metadata
    camera_metadata_t* meta = allocate_camera_metadata(5, 50);
    int64_t ts = timestamp;
    add_camera_metadata_entry(meta, ANDROID_SENSOR_TIMESTAMP, &ts, 1);
    
    size_t metaSize = get_camera_metadata_size(meta);
    results[0].result.metadata.resize(metaSize);
    memcpy(results[0].result.metadata.data(), meta, metaSize);
    free_camera_metadata(meta);
    
    mCallback->processCaptureResult(results);
    
    return CameraStatus::OK;
}

void VirtualCameraSession::fillTestPattern(void* /*data*/, int /*width*/, int /*height*/,
                                            int /*stride*/, int /*frameNumber*/) {
    // TODO: Implement when we add proper buffer access
    // For now, buffers are returned as-is (uninitialized)
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
