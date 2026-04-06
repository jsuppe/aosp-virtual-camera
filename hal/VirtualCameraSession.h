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
// HandleImporter for buffer mapping
#include <HandleImporter.h>
#include <utils/Thread.h>
#include <system/graphics.h>
#include <cutils/native_handle.h>
#include <ui/Rect.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "VirtualCameraFrameSource.h"
#include "VirtualCameraFrameSourceV2.h"
#include "../v2-shared-memory/HalInterface.h"

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
    VirtualCameraSession(const std::shared_ptr<ICameraDeviceCallback>& callback,
                         std::shared_ptr<VirtualCameraFrameSource> frameSource,
                         std::shared_ptr<VirtualCameraFrameSourceV2> frameSourceV2);
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
    // HandleImporter for buffer mapping (shared across all sessions)
    static ::android::hardware::camera::common::V1_0::helper::HandleImporter sHandleImporter;
    
    // Buffer cache: maps (streamId, bufferId) -> buffer_handle_t
    // Framework sends empty handles after first request; we must cache them
    using BufferKey = std::pair<int32_t, uint64_t>;  // (streamId, bufferId)
    struct BufferKeyHash {
        size_t operator()(const BufferKey& k) const {
            return std::hash<int32_t>()(k.first) ^ (std::hash<uint64_t>()(k.second) << 1);
        }
    };
    std::unordered_map<BufferKey, buffer_handle_t, BufferKeyHash> mBufferCache;
    
    // Import a buffer into the cache (if not already cached)
    // Returns the cached handle (or nullptr if buffer is empty and not cached)
    buffer_handle_t importBuffer(const StreamBuffer& buffer);
    
    // Remove buffers from cache
    void removeBuffersFromCache(const std::vector<BufferCache>& cachesToRemove);
    
    // Fill YUV buffer with test pattern or v1 renderer data
    void fillYuvBuffer(buffer_handle_t handle, int width, int height, int frameNumber);

    // Fill output buffer from v2 zero-copy source (AHardwareBuffer → gralloc buffer)
    // Dispatches to YUV passthrough or RGBA→YUV conversion based on source format.
    bool fillBufferFromV2(buffer_handle_t handle, int width, int height);

    // Direct YUV plane copy — renderer provided matching YUV format (fastest path)
    bool fillBufferFromV2Yuv(buffer_handle_t handle, int width, int height,
                             const virtualcamera::HalInterface::AcquiredFrame& frame);

    // RGBA→YUV conversion — renderer provided RGBA, consumer needs YUV
    bool fillBufferFromV2Rgba(buffer_handle_t handle, int width, int height,
                              const virtualcamera::HalInterface::AcquiredFrame& frame);

    // Process a single capture request
    CameraStatus processSingleRequest(const CaptureRequest& request);
    
    std::shared_ptr<ICameraDeviceCallback> mCallback;
    std::mutex mLock;
    std::atomic<bool> mClosed{false};
    
    // Configured streams
    std::unordered_map<int32_t, Stream> mStreams;
    
    // Frame counter for test pattern animation
    std::atomic<int> mFrameCounter{0};
    
    // v1 frame source for renderer-provided frames (shared with provider)
    std::shared_ptr<VirtualCameraFrameSource> mFrameSource;

    // v2 zero-copy frame source (shared with provider)
    std::shared_ptr<VirtualCameraFrameSourceV2> mFrameSourceV2;

    // Track last acquired frame timestamp (to avoid re-acquiring same frame)
    uint64_t mLastFrameTimestamp{0};
};

}  // namespace
