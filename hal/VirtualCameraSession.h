/*
 * VirtualCameraSession - Camera session that generates test patterns
 * 
 * This is a minimal implementation for testing. In production, this would
 * receive frames from an external renderer app via BufferQueue.
 */
#pragma once

#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/device/BnCameraDeviceSession.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/Stream.h>
// FMQ not used in minimal implementation
// #include <fmq/AidlMessageQueue.h>
#include <utils/Thread.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace aidl::android::hardware::camera::provider::implementation {

using CameraStatus = ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::camera::device::BnCameraDeviceSession;
using ::aidl::android::hardware::camera::device::BufferCache;
using ::aidl::android::hardware::camera::device::CameraMetadata;
using ::aidl::android::hardware::camera::device::CameraOfflineSessionInfo;
using ::aidl::android::hardware::camera::device::CaptureRequest;
using ::aidl::android::hardware::camera::device::CaptureResult;
using ::aidl::android::hardware::camera::device::HalStream;
using ::aidl::android::hardware::camera::device::ICameraDeviceCallback;
using ::aidl::android::hardware::camera::device::ICameraOfflineSession;
using ::aidl::android::hardware::camera::device::NotifyMsg;
using ::aidl::android::hardware::camera::device::RequestTemplate;
using ::aidl::android::hardware::camera::device::ShutterMsg;
using ::aidl::android::hardware::camera::device::Stream;
using ::aidl::android::hardware::camera::device::StreamBuffer;
using ::aidl::android::hardware::camera::device::StreamConfiguration;
using ::aidl::android::hardware::common::fmq::MQDescriptor;
using ::aidl::android::hardware::common::fmq::SynchronizedReadWrite;

class VirtualCameraSession : public BnCameraDeviceSession {
public:
    VirtualCameraSession(const std::shared_ptr<ICameraDeviceCallback>& callback);
    ~VirtualCameraSession() override;

    // ICameraDeviceSession interface
    ndk::ScopedAStatus close() override;
    
    ndk::ScopedAStatus configureStreams(
            const StreamConfiguration& requestedConfiguration,
            std::vector<HalStream>* halStreams) override;
    
    ndk::ScopedAStatus constructDefaultRequestSettings(
            RequestTemplate type,
            CameraMetadata* metadata) override;
    
    ndk::ScopedAStatus flush() override;
    
    ndk::ScopedAStatus getCaptureRequestMetadataQueue(
            MQDescriptor<int8_t, SynchronizedReadWrite>* queue) override;
    
    ndk::ScopedAStatus getCaptureResultMetadataQueue(
            MQDescriptor<int8_t, SynchronizedReadWrite>* queue) override;
    
    ndk::ScopedAStatus isReconfigurationRequired(
            const CameraMetadata& oldSessionParams,
            const CameraMetadata& newSessionParams,
            bool* required) override;
    
    ndk::ScopedAStatus processCaptureRequest(
            const std::vector<CaptureRequest>& requests,
            const std::vector<BufferCache>& cachesToRemove,
            int32_t* numRequestsProcessed) override;
    
    ndk::ScopedAStatus signalStreamFlush(
            const std::vector<int32_t>& streamIds,
            int32_t streamConfigCounter) override;
    
    ndk::ScopedAStatus switchToOffline(
            const std::vector<int32_t>& streamsToKeep,
            CameraOfflineSessionInfo* offlineSessionInfo,
            std::shared_ptr<ICameraOfflineSession>* session) override;
    
    ndk::ScopedAStatus repeatingRequestEnd(
            int32_t frameNumber,
            const std::vector<int32_t>& streamIds) override;

private:
    // Fill buffer with test pattern (color bars)
    void fillTestPattern(void* data, int width, int height, int stride, int frameNumber);
    
    // Process a single capture request
    CameraStatus processSingleRequest(const CaptureRequest& request);
    
    std::shared_ptr<ICameraDeviceCallback> mCallback;
    std::mutex mLock;
    std::atomic<bool> mClosed{false};
    
    // Configured streams
    std::unordered_map<int32_t, Stream> mStreams;
    
    // Frame counter for test pattern animation
    std::atomic<int> mFrameCounter{0};
};

}  // namespace
