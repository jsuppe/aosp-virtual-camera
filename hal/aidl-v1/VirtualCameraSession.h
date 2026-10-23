/*
 * VirtualCameraSession - AIDL V1 Camera Session Adapter
 *
 * Thin adapter implementing BnCameraDeviceSession (V1). Owns the buffer cache
 * and AIDL type conversions. Delegates all frame filling and metadata building
 * to core/.
 *
 * V1 interface: does NOT include configureStreamsV2 or getSessionCharacteristics.
 */
#pragma once

#include "ProducerCapabilities.h"
#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/device/BnCameraDeviceSession.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/Stream.h>
#include <HandleImporter.h>
#include <utils/Thread.h>
#include <system/graphics.h>
#include <cutils/native_handle.h>
#include <ui/Rect.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>

// Core types
#include "VirtualCameraFrameSource.h"
#include "VirtualCameraFrameSourceV2.h"

namespace virtualcamera {
class AidlFrameSource;
}
#include "MetadataBuilder.h"
#include "HalInterface.h"

namespace aidl::android::hardware::camera::provider::implementation {

class VirtualCameraStableHal;

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
    VirtualCameraSession(
            const std::shared_ptr<ICameraDeviceCallback>& callback, int slot,
            std::shared_ptr<::virtualcamera::VirtualCameraFrameSource> frameSource,
            std::shared_ptr<::virtualcamera::VirtualCameraFrameSourceV2> frameSourceV2,
            std::shared_ptr<::virtualcamera::AidlFrameSource> aidlSource = nullptr,
            std::shared_ptr<VirtualCameraStableHal> stableSource = nullptr,
                         std::shared_ptr<const ::virtualcamera::ProducerCapabilities> capabilities = nullptr);
    ~VirtualCameraSession() override;

    // Local diagnostic oracle: dispatches that can write a framework target.
    // Not a Binder API or a count of driver-level GPU operations.
    uint64_t outputRenderAttempts() const { return mOutputRenderAttempts.load(); }
    int64_t sourceConfigurationToken() const { return mConfigurationToken; }

    // ICameraDeviceSession V1 interface
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

    // Import a buffer from AIDL StreamBuffer into the cache (if not already cached).
    // This stays in the adapter because it touches AIDL StreamBuffer type directly.
    // Returns the cached handle (or nullptr if buffer is empty and not cached).
    buffer_handle_t importBuffer(const StreamBuffer& buffer);

    // Remove buffers from cache
    void removeBuffersFromCache(const std::vector<BufferCache>& cachesToRemove);

    // Process a single capture request (adapter orchestration)
    CameraStatus processSingleRequest(const CaptureRequest& request, uint64_t cancelEpoch);

    // Frame pacing: pick up ANDROID_CONTROL_AE_TARGET_FPS_RANGE from request
    // settings (only present when they change) and block until the next
    // frame slot. Returns the slot's timestamp (ns, steady clock).
    bool updateTargetFps(const CameraMetadata& settings);
    int64_t paceFrame(uint64_t cancelEpoch);

    std::shared_ptr<ICameraDeviceCallback> mCallback;
    std::shared_ptr<const ::virtualcamera::ProducerCapabilities> mCapabilities;
    bool mEnforceCapabilities = false;
    std::vector<::virtualcamera::ProducerCapabilities::Output> mOutputs;
    ::virtualcamera::ProducerMode mSourceMode;
    int mSlot = 0;   // which virtual camera (producer) this session serves
    std::mutex mLock;
    std::atomic<bool> mClosed{false};
    // Serialize stream/cache/settings access; cancellation never needs this
    // lock, so flush/close can wake a request waiting for a producer.
    std::recursive_mutex mRequestLock;
    std::mutex mWaitLock;
    std::condition_variable mWaitCv;
    std::atomic<uint64_t> mCancelEpoch{0};
    std::atomic<bool> mFlushing{false};
    uint64_t mLastSourceSequence = 0;
    int64_t mSessionIdentity = 0, mConfigurationToken = 0;
    bool mStartupWaitPending = true;
    std::shared_ptr<VirtualCameraStableHal> mStableSource;

    // Configured streams
    std::unordered_map<int32_t, Stream> mStreams;

    // Frame counter
    std::atomic<int> mFrameCounter{0};
    std::atomic<uint64_t> mOutputRenderAttempts{0};

    // Pacing state (touched only on the request path)
    int32_t mTargetFps = 30;
    int64_t mNextFrameNs = 0;
    int32_t mJpegQuality = 90;
    bool mHaveSettings = false;   // first request after configure must carry settings
    bool mForceCpuYuv = false;    // vendor.vcam.yuv.cpu=1: measurement baseline

    // Fill-path statistics (logged periodically)
    struct FillStats {
        uint64_t gpuRgba = 0, gpuYuv = 0, cpuYuv = 0, jpeg = 0;
        int64_t gpuYuvNs = 0, cpuYuvNs = 0, jpegNs = 0;
    } mStats;

    // Core frame sources (shared with provider)
    std::shared_ptr<::virtualcamera::VirtualCameraFrameSource> mFrameSource;
    std::shared_ptr<::virtualcamera::VirtualCameraFrameSourceV2> mFrameSourceV2;
    std::shared_ptr<::virtualcamera::AidlFrameSource> mAidlSource;

};

}  // namespace aidl::android::hardware::camera::provider::implementation
