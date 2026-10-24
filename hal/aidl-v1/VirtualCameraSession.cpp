/*
 * VirtualCameraSession - AIDL V1 Adapter Implementation
 *
 * Owns the buffer cache and AIDL type conversions. Delegates all frame
 * filling to core::fillYuvBufferFromRenderer / core::fillBufferFromV2
 * and metadata building to core::MetadataBuilder.
 */

#define LOG_TAG "VirtualCameraSession"

#include "VirtualCameraSession.h"
#include "AcquireFenceWait.h"

#include <log/log.h>
#include <system/camera_metadata.h>
#include <aidl/android/hardware/camera/device/ErrorCode.h>
#include <aidl/android/hardware/camera/device/ErrorMsg.h>
#include <aidl/android/hardware/camera/device/BufferStatus.h>
#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidlcommonsupport/NativeHandle.h>

#include <android/hardware_buffer.h>
#include <android/sync.h>
#include <android-base/properties.h>
#include <hardware/gralloc.h>
#include <ui/GraphicBufferMapper.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include <set>

// Core frame filling
#include "FrameFiller.h"
#include "JpegEncoder.h"
#include "MetadataBuilder.h"
#include "AidlFrameSource.h"
#include "VirtualCameraStableHal.h"
#ifdef VCAM_GPU_COMPOSITOR
#include "GpuCompositor.h"
#endif

namespace aidl::android::hardware::camera::provider::implementation {

using ::aidl::android::hardware::camera::device::BufferStatus;
using ::aidl::android::hardware::camera::device::ErrorCode;
using ::aidl::android::hardware::camera::device::ErrorMsg;
using ::aidl::android::hardware::graphics::common::BufferUsage;
using ::android::hardware::camera::common::V1_0::helper::HandleImporter;

// Static HandleImporter instance (shared across all sessions)
HandleImporter VirtualCameraSession::sHandleImporter;

VirtualCameraSession::VirtualCameraSession(
        const std::shared_ptr<ICameraDeviceCallback>& callback, int slot,
        std::shared_ptr<::virtualcamera::VirtualCameraFrameSource> frameSource,
        std::shared_ptr<::virtualcamera::VirtualCameraFrameSourceV2> frameSourceV2,
        std::shared_ptr<::virtualcamera::AidlFrameSource> aidlSource,
        std::shared_ptr<VirtualCameraStableHal> stableSource,
        std::shared_ptr<const ::virtualcamera::ProducerCapabilities> capabilities)
    : mCallback(callback),
      mSlot(slot),
      mFrameSource(frameSource),
      mFrameSourceV2(frameSourceV2),
      mAidlSource(aidlSource) {
    mStableSource = std::move(stableSource);
    mEnforceCapabilities = capabilities != nullptr;
    mCapabilities = capabilities ? std::move(capabilities) : std::make_shared<::virtualcamera::ProducerCapabilities>(::virtualcamera::ProducerCapabilities::diagnostic());
#ifdef VCAM_STABLE_AIDL
    if (auto* hal = mStableSource ? mStableSource.get() : VirtualCameraStableHal::get()) {
        mSessionIdentity = hal->allocateSessionIdentity();
    }
#endif
    mForceCpuYuv = ::android::base::GetBoolProperty("vendor.vcam.yuv.cpu", false);
    mProfile.enable(::android::base::GetIntProperty<int>("vendor.vcam.profile",0),
                    mSlot,mSessionIdentity);
    if(mProfile.enabled() && ::android::base::GetBoolProperty("ro.debuggable",false))
        mProfileDeadlineFrames=std::clamp(::android::base::GetIntProperty<int>(
                "vendor.vcam.profile.deadline",1),1,2);
    mProfileClearOutput = mProfile.enabled() &&
            ::android::base::GetBoolProperty("ro.debuggable", false) &&
            ::android::base::GetBoolProperty("vendor.vcam.profile.clear", false);
    if (mProfile.enabled()) ALOGI("PROFILE_CLEAR_OUTPUT enabled=%d", mProfileClearOutput);
    ALOGI("VirtualCameraSession created (slot %d; AIDL V1 adapter, v1 + v2 frame sources)%s",
          mSlot, mForceCpuYuv ? " [vendor.vcam.yuv.cpu=1: CPU YUV converter forced]" : "");
}

VirtualCameraSession::~VirtualCameraSession() {
    close();  // cancel/drain before releasing cached framework handles
    ALOGI("VirtualCameraSession destroyed, clearing %zu cached buffers", mBufferCache.size());

    // Free all cached buffer handles
    for (auto& pair : mBufferCache) {
        if (pair.second != nullptr) {
            sHandleImporter.freeBuffer(pair.second);
        }
    }
    mBufferCache.clear();

}

ndk::ScopedAStatus VirtualCameraSession::close() {
    ALOGI("Session close requested");
    const bool wasClosed = mClosed.exchange(true);
    mCancelEpoch.fetch_add(1);
    mWaitCv.notify_all();
    std::lock_guard<std::recursive_mutex> requests(mRequestLock);
    if (wasClosed) return ndk::ScopedAStatus::ok();
#ifdef VCAM_AIDL_SOURCE
    if (mAidlSource) {
        mAidlSource->sessionClosed();
    }
#endif
#ifdef VCAM_STABLE_AIDL
    auto* hal = mStableSource ? mStableSource.get() : VirtualCameraStableHal::get();
    if (hal) hal->notifyCameraClosed(mSlot, mSessionIdentity, mConfigurationToken);
#endif
    mClosed = true;
    if (!mProfile.write("/data/vendor/vcam-profile"))
        ALOGE("Could not export opt-in frame profile");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::configureStreams(
        const StreamConfiguration& requestedConfiguration,
        std::vector<HalStream>* halStreams) {

    std::lock_guard<std::recursive_mutex> requests(mRequestLock);
    std::lock_guard<std::mutex> lock(mLock);

    if (mClosed) {
        return ndk::ScopedAStatus::fromServiceSpecificError(
                static_cast<int32_t>(CameraStatus::INTERNAL_ERROR));
    }

    // Same rule as ICameraDevice.isStreamCombinationSupported: only advertised
    // formats/sizes, no input streams, no stream use cases (none advertised).
    {
        std::vector<::virtualcamera::MetadataBuilder::StreamDesc> descs;
        std::set<int> streamIds;
        for (const auto& s : requestedConfiguration.streams) {
            if(s.id<0 || !streamIds.insert(s.id).second)return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(CameraStatus::ILLEGAL_ARGUMENT));
            descs.push_back({static_cast<int>(s.format), s.width, s.height,
                             static_cast<int64_t>(s.useCase),
                             s.streamType == ::aidl::android::hardware::camera::device::StreamType::INPUT,
                             static_cast<int>(s.rotation)});
        }
        if (!::virtualcamera::MetadataBuilder::isStreamCombinationSupported(descs, mCapabilities.get())) {
            ALOGE("configureStreams: unsupported stream combination");
            return ndk::ScopedAStatus::fromServiceSpecificError(
                    static_cast<int32_t>(CameraStatus::ILLEGAL_ARGUMENT));
        }
    }


    // Clear old streams and buffer cache
    mStreams.clear();
    mHaveSettings = false;
    mStartupWaitPending = true;
    for (auto& pair : mBufferCache) {
        if (pair.second != nullptr) {
            sHandleImporter.freeBuffer(pair.second);
        }
    }
    mBufferCache.clear();
    halStreams->clear();

    ALOGI("Configuring %zu streams", requestedConfiguration.streams.size());

    mOutputs.clear();
    for(const auto& stream:requestedConfiguration.streams)mOutputs.push_back({int(stream.format),stream.width,stream.height});
    const auto* selected=mCapabilities->select(mOutputs);
    if(!selected)return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(CameraStatus::ILLEGAL_ARGUMENT));
    mSourceMode=*selected;mTargetFps=std::clamp(30,selected->minFps,selected->maxFps);mNextFrameNs=0;
#ifdef VCAM_STABLE_AIDL
    if(mEnforceCapabilities) {
        auto* h=mStableSource?mStableSource.get():VirtualCameraStableHal::get();
        if(!h||!h->registrationCurrent(mSlot,mCapabilities->registration))return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(CameraStatus::INTERNAL_ERROR));
    }
#endif
    for (const auto& stream : requestedConfiguration.streams) {
        ALOGI("Stream %d: %dx%d format=%d",
              stream.id, stream.width, stream.height,
              static_cast<int>(stream.format));

        // Resolve the buffer format. IMPLEMENTATION_DEFINED (0x22) lets the HAL
        // pick; we choose RGBA_8888 so the producer's GPU-rendered RGBA frame
        // can be blitted straight in with ZERO color conversion (GpuCompositor).
        constexpr int kImplDefined = 0x22;   // HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED
        constexpr int kRGBA8888 = 0x1;       // HAL_PIXEL_FORMAT_RGBA_8888
        constexpr int kBlob = 0x21;          // HAL_PIXEL_FORMAT_BLOB (JPEG)
        int resolvedFormat = static_cast<int>(stream.format);
        if (resolvedFormat == kImplDefined) {
            resolvedFormat = kRGBA8888;
        }
        const bool rgbaPath = (resolvedFormat == kRGBA8888);
        const bool blobPath = (resolvedFormat == kBlob);

        // Store stream config (with the resolved format, so the fill path knows
        // what the allocated output buffer actually is).
        Stream stored = stream;
        stored.format = static_cast<decltype(stream.format)>(resolvedFormat);
        mStreams[stream.id] = stored;

        // Return HAL stream configuration
        HalStream halStream;
        halStream.id = stream.id;
        halStream.overrideFormat = static_cast<decltype(halStream.overrideFormat)>(resolvedFormat);
        // RGBA path: allocate GPU-renderable buffers (GpuCompositor writes them
        // via an FBO renderbuffer) — no CPU_WRITE, so gralloc is free to pick a
        // GPU-optimal layout. YUV/other paths keep CPU_WRITE for the CPU filler.
        int64_t producerUsage = static_cast<int64_t>(BufferUsage::CAMERA_OUTPUT);
        if (rgbaPath) {
            producerUsage |= static_cast<int64_t>(BufferUsage::GPU_RENDER_TARGET);
        } else {
            producerUsage |= static_cast<int64_t>(BufferUsage::CPU_WRITE_OFTEN);
        }
        halStream.producerUsage = static_cast<BufferUsage>(producerUsage);
        halStream.consumerUsage = static_cast<BufferUsage>(0);
        halStream.maxBuffers = blobPath ? 2 : 4;   // stills stall; keep the queue short
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

#ifdef VCAM_AIDL_SOURCE
    // Platform relay mode: create the BufferQueue and deliver the Surface to
    // the registered producer app (via VirtualCameraService in system_server).
    if (mAidlSource && !requestedConfiguration.streams.empty()) {
        mAidlSource->configureStreams(mSourceMode.width, mSourceMode.height, mTargetFps);
    }
#endif
#ifdef VCAM_STABLE_AIDL
    // Stable-AIDL relay: tell the platform service the stream shape so it can
    // create the BufferQueue and hand its Surface to the producer app.
    if (!requestedConfiguration.streams.empty()) {
        if (auto* hal = mStableSource ? mStableSource.get() : VirtualCameraStableHal::get()) {
            mConfigurationToken = hal->notifyStreamsConfigured(
                    mSlot, mSessionIdentity, mSourceMode.width, mSourceMode.height, mTargetFps);
            if (mConfigurationToken <= 0) {
                return ndk::ScopedAStatus::fromServiceSpecificError(
                        static_cast<int32_t>(CameraStatus::INTERNAL_ERROR));
            }
        }
    }
#endif

    // Also publish format request to v1 renderer
    if (mFrameSource && !requestedConfiguration.streams.empty()) {
        const auto& primary = requestedConfiguration.streams[0];
        mFrameSource->requestFormat(::virtualcamera::FORMAT_YUV_420,
                                    primary.width, primary.height);
    }

    ALOGI("Streams configured successfully");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::constructDefaultRequestSettings(
        RequestTemplate type,
        CameraMetadata* metadata) {

    (void)type;  // All templates return the same minimal settings
    metadata->metadata = ::virtualcamera::MetadataBuilder::buildDefaultRequestSettings(mCapabilities.get());
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::flush() {
    ALOGI("Flush requested");
    mFlushing = true;
    mCancelEpoch.fetch_add(1);
    mWaitCv.notify_all();
    std::lock_guard<std::recursive_mutex> requests(mRequestLock);
    mNextFrameNs = 0;
    mFlushing = false;
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
        // DIAG (rate-limited): reveal what the framework actually sent so we can
        // see why output buffers arrive without fds on first use.
        static std::atomic<int> sNoHandleCount{0};
        if ((sNoHandleCount++ % 300) == 0) {
            ALOGE("importBuffer: no fds (streamId=%d bufferId=%lu ints=%zu status=%d cache=%zu) [x%d]",
                  buffer.streamId, (unsigned long)buffer.bufferId,
                  buffer.buffer.ints.size(), (int)buffer.status,
                  mBufferCache.size(), sNoHandleCount.load());
        }
        return nullptr;
    }

    // Convert AIDL NativeHandle to native_handle_t, then to buffer_handle_t
    native_handle_t* nativeHandle = ::android::makeFromAidl(buffer.buffer);
    if (nativeHandle == nullptr || nativeHandle->numFds < 1) {
        ALOGE("importBuffer: Invalid handle from AIDL (numFds=%d)",
              nativeHandle ? nativeHandle->numFds : -1);
        if (nativeHandle) {
            native_handle_delete(nativeHandle);
        }
        return nullptr;
    }

    // Import the buffer using HandleImporter
    buffer_handle_t bufHandle = nativeHandle;
    if (!sHandleImporter.importBuffer(bufHandle)) {
        ALOGE("importBuffer: HandleImporter.importBuffer failed");
        native_handle_delete(nativeHandle);
        return nullptr;
    }

    // makeFromAidl borrows the parcel's fds; the mapper cloned the imported
    // handle. Delete our temporary wrapper, but NEVER close borrowed fds.
    const int inputFdCount = nativeHandle->numFds;
    native_handle_delete(nativeHandle);

    // Profile-only: inspect actual imported allocations, not inferred Surface usage.
    // Bound logging to the initial pool; preserve getter errors rather than treating
    // unavailable metadata as a valid zero/linear modifier.
    if (mProfile.enabled() && mBufferCache.size() < 8 &&
            ::android::base::GetBoolProperty("ro.debuggable", false)) {
        auto& mapper = ::android::GraphicBufferMapper::get();
        uint64_t usage = 0, modifier = 0, bytes = 0;
        uint32_t fourcc = 0;
        int usageStatus = mapper.getUsage(bufHandle, &usage);
        int modifierStatus = mapper.getPixelFormatModifier(bufHandle, &modifier);
        int sizeStatus = mapper.getAllocationSize(bufHandle, &bytes);
        int formatStatus = mapper.getPixelFormatFourCC(bufHandle, &fourcc);
        ALOGI("PROFILE_ALLOCATION stream=%d buffer=%lu usage=0x%lx modifier=0x%lx "
              "fourcc=0x%x bytes=%lu status=%d,%d,%d,%d",
              buffer.streamId, (unsigned long)buffer.bufferId,
              (unsigned long)usage, (unsigned long)modifier, fourcc,
              (unsigned long)bytes, usageStatus, modifierStatus, formatStatus, sizeStatus);
    }

    // Cache it
    mBufferCache[key] = bufHandle;

    if (mBufferCache.size() <= 8) {  // Only log first few
        ALOGI("importBuffer: Cached buffer (streamId=%d, bufferId=%lu, numFds=%d)",
              buffer.streamId, (unsigned long)buffer.bufferId, inputFdCount);
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

    std::lock_guard<std::recursive_mutex> requestsLock(mRequestLock);
    if (mClosed || mFlushing) {
        *numRequestsProcessed = 0;
        return ndk::ScopedAStatus::fromServiceSpecificError(
                static_cast<int32_t>(CameraStatus::INTERNAL_ERROR));
    }

    if (!cachesToRemove.empty()) {
        removeBuffersFromCache(cachesToRemove);
    }

    *numRequestsProcessed = 0;
    const uint64_t cancelEpoch = mCancelEpoch.load();

    for (const auto& request : requests) {
        // Validate before any callback fires: a request with no output
        // buffers, an unknown stream, or a buffer we can neither import nor
        // find in the cache is an ILLEGAL_ARGUMENT (VTS: InvalidBuffer).
        bool valid = !request.outputBuffers.empty() && request.inputBuffer.streamId == -1;
        // Settings: the first request after configureStreams must carry
        // them (later ones may be empty = "unchanged").
        if (request.settings.metadata.empty() && request.fmqSettingsSize == 0 && !mHaveSettings) {
            valid = false;
        }
        for (const auto& b : request.outputBuffers) {
            if (!valid) break;
            // A sync-fence NativeHandle is null or exactly one valid fd,
            // without integer payload. Reject malformed handles before
            // taking ownership or emitting any capture callbacks.
            if (!b.acquireFence.ints.empty() || b.acquireFence.fds.size() > 1 ||
                    (!b.acquireFence.fds.empty() && b.acquireFence.fds[0].get() < 0)) {
                valid = false;
            } else if (mStreams.find(b.streamId) == mStreams.end()) valid = false;
            else if (b.buffer.fds.empty() &&
                     mBufferCache.find(BufferKey(b.streamId, b.bufferId)) == mBufferCache.end()) {
                valid = false;
            }
        }
        if (!valid) {
            ALOGE("processCaptureRequest: invalid request %d (%zu buffers)",
                  request.frameNumber, request.outputBuffers.size());
            return ndk::ScopedAStatus::fromServiceSpecificError(
                    static_cast<int32_t>(CameraStatus::ILLEGAL_ARGUMENT));
        }
        if(!updateTargetFps(request.settings))return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(CameraStatus::ILLEGAL_ARGUMENT));
        CameraStatus status = processSingleRequest(request, cancelEpoch);
        if (status != CameraStatus::OK) {
            ALOGE("Failed to process request %d", request.frameNumber);
            return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(status));
        }
        (*numRequestsProcessed)++;
    }

    return ndk::ScopedAStatus::ok();
}

bool VirtualCameraSession::updateTargetFps(const CameraMetadata& settings) {
#ifdef VCAM_STABLE_AIDL
    auto* hal=mStableSource?mStableSource.get():VirtualCameraStableHal::get();
    if(mEnforceCapabilities && (!hal||!hal->registrationCurrent(mSlot,mCapabilities->registration)))return false;
#endif
    if(settings.metadata.empty())return true;
    const auto* meta=reinterpret_cast<const camera_metadata_t*>(settings.metadata.data());
    const size_t size=settings.metadata.size();if(validate_camera_metadata_structure(meta,&size)!=0)return false;
    camera_metadata_ro_entry_t e;
    int fps=mTargetFps;
    const ::virtualcamera::ProducerMode* selected=&mSourceMode;
    if(find_camera_metadata_ro_entry(meta,ANDROID_CONTROL_AE_TARGET_FPS_RANGE,&e)==0) {
        if(e.count!=2)return false;
        selected=mCapabilities->select(mOutputs,e.data.i32[0],e.data.i32[1]);
        if(!selected)return false;
        fps=std::min(e.data.i32[1],selected->maxFps);
    }
    if(fps!=mTargetFps||selected->id!=mSourceMode.id) {
#ifdef VCAM_STABLE_AIDL
        if(!hal)return false;
        int64_t token=hal->notifyStreamsConfigured(mSlot,mSessionIdentity,selected->width,selected->height,fps);
        if(token<=0)return false;
        mConfigurationToken=token;
#endif
        mSourceMode=*selected;mTargetFps=fps;mNextFrameNs=0;mStartupWaitPending=true;
        ALOGI("Negotiated source mode=%d %dx%d @%d token=%lld",selected->id,selected->width,selected->height,fps,(long long)mConfigurationToken);
    }
    if(find_camera_metadata_ro_entry(meta,ANDROID_JPEG_QUALITY,&e)==0&&e.count>=1)mJpegQuality=e.data.u8[0];
    mHaveSettings=true;return true;
}

int64_t VirtualCameraSession::paceFrame(uint64_t cancelEpoch) {
    using namespace std::chrono;
    const int64_t interval = (1000000000LL + mTargetFps - 1) / mTargetFps;
    int64_t now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    if (mNextFrameNs == 0 || now > mNextFrameNs + interval) {
        mNextFrameNs = now;   // first frame, or we fell more than a slot behind: resync
    }
    if (now < mNextFrameNs) {
        std::unique_lock<std::mutex> wait(mWaitLock);
        mWaitCv.wait_for(wait, nanoseconds(mNextFrameNs - now), [&] {
            return mClosed || mCancelEpoch.load() != cancelEpoch;
        });
    }
    if (mClosed || mCancelEpoch.load() != cancelEpoch) return 0;
    const int64_t slot = mNextFrameNs;
    mNextFrameNs += interval;
    return slot;
}

CameraStatus VirtualCameraSession::processSingleRequest(const CaptureRequest& request,
                                                       uint64_t cancelEpoch) {
    using Profile=::virtualcamera::FrameProfile;
    Profile::Scope profile(mProfile,request.frameNumber,mTargetFps,
                           mSourceMode.width,mSourceMode.height,mConfigurationToken);
    // Reserve the dependencies required to return UNTOUCHED output buffers on
    // error before any wait, import, shutter or target write. Never replace a
    // failed dup with the no-fence sentinel. The request is not accepted if we
    // cannot preserve its dependencies; INTERNAL_ERROR makes the session fatal.
    std::vector<ndk::ScopedFileDescriptor> pendingFences(request.outputBuffers.size());
    for (size_t i = 0; i < request.outputBuffers.size(); ++i) {
        const auto& fence = request.outputBuffers[i].acquireFence;
        if (!fence.fds.empty()) {
            int fd = ::dup(fence.fds[0].get());
            if (fd < 0) {
                ALOGE("Cannot preserve destination acquire fence for request %d", request.frameNumber);
                profile.fail(Profile::FenceDup,Profile::Import);
                profile.finish();
                close();
                return CameraStatus::INTERNAL_ERROR;
            }
            pendingFences[i].set(fd);
        }
    }

    // Importing only establishes handle ownership/metadata; it does not touch
    // image contents. Accepted failed requests must still cache newly supplied
    // buffers, since the framework may refer to them by ID on its next request.
    for (const auto& buffer : request.outputBuffers) {
        if (importBuffer(buffer) == nullptr) {
            profile.fail(Profile::InvalidImport,Profile::Import);
            return CameraStatus::ILLEGAL_ARGUMENT;
        }
    }
    profile.mark(Profile::Import);

    auto cancelled = [&] { return mClosed || mCancelEpoch.load() != cancelEpoch; };
    // This helper returns only UNTOUCHED buffers. A no-source capture is a
    // whole-request failure, not ERROR_DEVICE and not a successful blank frame.
    auto failUntouched = [&](Profile::Failure reason,Profile::Phase phase) {
        profile.fail(reason,phase);
        std::vector<NotifyMsg> messages(1);
        messages[0].set<NotifyMsg::Tag::error>(ErrorMsg{
            .frameNumber = request.frameNumber, .errorStreamId = -1,
            .errorCode = ErrorCode::ERROR_REQUEST,
        });
        mCallback->notify(messages);
        CaptureResult result;
        result.frameNumber = request.frameNumber;
        result.inputBuffer.streamId = -1;
        result.partialResult = 0;
        for (size_t j = 0; j < request.outputBuffers.size(); ++j) {
            StreamBuffer buffer;
            buffer.streamId = request.outputBuffers[j].streamId;
            buffer.bufferId = request.outputBuffers[j].bufferId;
            buffer.status = BufferStatus::ERROR;
            if (pendingFences[j].get() >= 0) {
                buffer.releaseFence.fds.emplace_back(pendingFences[j].release());
            }
            result.outputBuffers.push_back(std::move(buffer));
        }
        std::vector<CaptureResult> results;
        results.push_back(std::move(result));
        mCallback->processCaptureResult(results);
        profile.mark(Profile::Callback);
        return CameraStatus::OK;
    };

    // Destination dependencies precede every possible target write. Preserve
    // failed/unvisited fences when returning untouched buffers.
    for (size_t i = 0; i < pendingFences.size(); ++i) {
        int fd = pendingFences[i].get();
        if (cancelled() || (fd >= 0 && sync_wait(fd, 100) != 0)) {
            return failUntouched(cancelled()?Profile::Cancelled:Profile::DestinationTimeout,
                                 Profile::DestinationWait);
        }
        pendingFences[i].set(-1);
    }
    profile.mark(Profile::DestinationWait);

    int64_t timestamp = paceFrame(cancelEpoch);
    if (cancelled()) return failUntouched(Profile::Cancelled,Profile::Pace);
    profile.mark(Profile::Pace);
    const int64_t frameDurationNs = (1000000000LL + mTargetFps - 1) / mTargetFps;
    int64_t producerTs = 0;
    AHardwareBuffer* src = nullptr;
    int acquireFence = -1;
#ifdef VCAM_STABLE_AIDL
    auto* hal = mStableSource ? mStableSource.get() : VirtualCameraStableHal::get();
    // A single reservation pins a single submission for the entire capture.
    if (!hal || mConfigurationToken <= 0)
        return failUntouched(Profile::StaleSource,Profile::SourceWait);
    // Destruction records ALL output read dependencies before waking enqueue.
    struct CaptureSource {
        VirtualCameraStableHal* hal;
        int slot;
        AHardwareBuffer* buffer = nullptr;
        ndk::ScopedFileDescriptor acquire;
        std::vector<int> fences;
        ~CaptureSource() {
            if (acquire.get() >= 0) fences.push_back(acquire.release());
            if (buffer) hal->releaseFrame(slot, buffer, std::move(fences));
        }
    } source{hal, mSlot, nullptr, {}, {}};
    auto& sourceFence = source.acquire;
    // Provisional reference policy: one requested frame period for normal
    // availability + readiness, with a 200ms minimum for the first attempt
    // after configuration. Measured cold GL startup exceeds a frame period.
    // Startup is NOT the steady-state latency objective; neither budget is
    // a producer-health/removal timeout or product-approved acceptance bound.
    // Polling is cancellable <=2ms (apart from actual driver calls).
    auto sourceBudget = std::chrono::nanoseconds(frameDurationNs * mProfileDeadlineFrames);
    if (mStartupWaitPending) {
        sourceBudget = std::max(sourceBudget, std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::milliseconds(200)));
        mStartupWaitPending = false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
            sourceBudget;
    // Diagnostic-only query after the readiness decision; never substitutes for a fence wait.
    auto recordFence = [&]() {
        if(!mProfile.enabled())return;
        int status=-2; int64_t signal=0;
        if(sourceFence.get()<0) status=1;
        else if(auto* info=sync_file_info(sourceFence.get())) {
            status=info->status;
            auto* fences=sync_get_fence_info(info);
            for(uint32_t i=0;i<info->num_fences;i++)
                signal=std::max(signal,static_cast<int64_t>(fences[i].timestamp_ns));
            sync_file_info_free(info);
        }
        profile.fence(std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline.time_since_epoch()).count(),signal,status);
    };
    uint64_t sequence = 0;
    while (hal && !cancelled()) {
        src = hal->acquireLatest(mSlot, &producerTs, &acquireFence,
                                &sequence, mLastSourceSequence, mConfigurationToken);
        if (src) break;
        std::unique_lock<std::mutex> wait(mWaitLock);
        if (std::chrono::steady_clock::now() >= deadline) break;
        mWaitCv.wait_until(wait, std::min(deadline, std::chrono::steady_clock::now() +
                std::chrono::milliseconds(2)), cancelled);
    }
    source.buffer = src;
    sourceFence.set(acquireFence);
    profile.source(sequence,producerTs);
    if (!src || cancelled()) return failUntouched(
            cancelled()?Profile::Cancelled:Profile::NoSource,Profile::SourceWait);
    profile.mark(Profile::SourceWait);
    // Consume submission identity, even if its fence or conversion later
    // fails. Timestamps may repeat and gralloc buffers may be reused.
    mLastSourceSequence = sequence;
    while (sourceFence.get() >= 0 && sync_wait(sourceFence.get(), 0) != 0) {
        if (cancelled() || !hal->isFrameCurrent(mSlot, src, sequence, mConfigurationToken) ||
                std::chrono::steady_clock::now() >= deadline) {
            recordFence();
            return failUntouched(cancelled()?Profile::Cancelled:
                    (!hal->isFrameCurrent(mSlot,src,sequence,mConfigurationToken)?
                     Profile::StaleSource:Profile::SourceFenceTimeout),Profile::SourceFenceWait);
        }
        std::unique_lock<std::mutex> wait(mWaitLock);
        mWaitCv.wait_until(wait, std::min(deadline, std::chrono::steady_clock::now() +
                std::chrono::milliseconds(2)), cancelled);
    }
    if (cancelled() || !hal->isFrameCurrent(mSlot, src, sequence, mConfigurationToken))
        return failUntouched(cancelled()?Profile::Cancelled:Profile::StaleSource,
                             Profile::SourceFenceWait);
    profile.mark(Profile::SourceFenceWait);
    recordFence();
    // Source fence completed on the CPU; downstream renderers need no wait.
    sourceFence.set(-1);
    acquireFence = -1;
#else
    // Legacy socket/platform-relay sources do not expose capture freshness.
    // Fail closed instead of silently using their cached/synthetic fallback.
    return failUntouched(Profile::NoSource,Profile::SourceWait);
#endif

    // Process each output buffer
    std::vector<StreamBuffer> outputBuffers;
    outputBuffers.reserve(request.outputBuffers.size());

    for (size_t i = 0; i < request.outputBuffers.size(); i++) {
        const auto& inBuffer = request.outputBuffers[i];
        int outputReleaseFence = -1;   // owned; attached to the result buffer
        bool filled = false;

        // Import/cache the buffer (adapter owns this — touches AIDL StreamBuffer)
        buffer_handle_t handle = importBuffer(inBuffer);

        // Get stream info for dimensions
        auto streamIt = mStreams.find(inBuffer.streamId);
        if (streamIt != mStreams.end() && handle != nullptr) {
            int width = streamIt->second.width;
            int height = streamIt->second.height;

#ifdef VCAM_STABLE_AIDL
            if (src && !cancelled() && hal->isFrameCurrent(mSlot, src, sequence, mConfigurationToken)) {
                        mOutputRenderAttempts++;
                        int dstFormat = static_cast<int>(streamIt->second.format);
                        int doneFence = -1;      // our GPU read+write completion
                        constexpr int kFmtBlob = 0x21;
                        constexpr int kFmtYcbcr420 = 0x23;
                        using clk = std::chrono::steady_clock;
                        if (dstFormat == kFmtBlob) {
                            // JPEG still: CPU encode straight from the RGBA
                            // source; the buffer is jpegMaxSize x 1 bytes.
                            const auto t0 = clk::now();
                            // BLOB buffers are bufferSize x 1 bytes; the framework
                            // sizes them per resolution from JPEG_MAX_SIZE and tells
                            // us in Stream.bufferSize. The trailer goes at the end.
                            const int32_t bs = streamIt->second.bufferSize;
                            const size_t cap = static_cast<size_t>(
                                    bs > 0 ? bs : ::virtualcamera::MetadataBuilder::kJpegMaxSize);
                            void* blob = sHandleImporter.lock(handle, GRALLOC_USAGE_SW_WRITE_OFTEN, cap);
                            if (blob) {
                                size_t n = ::virtualcamera::JpegEncoder::encodeToBlob(
                                        src, acquireFence, width, height, mJpegQuality, blob, cap);
                                sHandleImporter.unlock(handle);
                                filled = (n > 0);
                                if (filled) {
                                    mStats.jpeg++;
                                    mStats.jpegNs += std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count();
                                    if (mStats.jpeg <= 3 || mStats.jpeg % 100 == 0) {
                                        ALOGI("JPEG %dx%d q%d: %zu bytes in %.1f ms", width, height,
                                              mJpegQuality, n,
                                              std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count() / 1e6);
                                    }
                                }
                            }
                        }
#ifdef VCAM_GPU_COMPOSITOR
                        if (!filled && dstFormat == kFmtYcbcr420 && !mForceCpuYuv) {
                            // GPU color conversion into packed planes + CPU
                            // row copy (no per-pixel CPU work; see
                            // GpuCompositor::compositeToYcbcr for why not a
                            // direct YUV render target on this GL stack).
                            const auto t0 = clk::now();
                            auto dst = ::virtualcamera::lockYCbCrCompat(
                                    sHandleImporter, handle, GRALLOC_USAGE_SW_WRITE_OFTEN, width, height);
                            if (dst.y) {
                                filled = ::virtualcamera::GpuCompositor::get().compositeToYcbcr(
                                        src, acquireFence, dst, width, height);
                                sHandleImporter.unlock(handle);
                            }
                            if (filled) {
                                mStats.gpuYuv++;
                                mStats.gpuYuvNs += std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count();
                            }
                        }
                        if (!filled && dstFormat != kFmtBlob) {
                            // GPU fast path: RGBA dst -> passthrough blit, ZERO
                            // color conversion, no CPU touch, no CPU wait: the GPU
                            // waits on the producer's fence and hands back its own.
                            // Returns false for YUV or if the GPU stack is
                            // unavailable -> CPU fallback.
                            std::array<int64_t,6> gpuCpu{};
                            filled = ::virtualcamera::GpuCompositor::get().composite(
                                    src, acquireFence, handle, width, height,
                                    /*stride*/ width, dstFormat,
                                    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                                    AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
                                    &doneFence, mProfileClearOutput, mProfile.enabled() ? &gpuCpu : nullptr);
                            profile.gpuCpu(gpuCpu);
                            if (filled) mStats.gpuRgba++;
                        }
#endif
                        if (!filled && dstFormat != kFmtBlob) {
                            // CPU converter reads src directly: wait for the
                            // producer's render to land first.
                            const auto t0 = clk::now();
                            if (::virtualcamera::acquireFenceReady(acquireFence,
                                    [](int) { return false; },
                                    [](int fd, int ms) { return sync_wait(fd, ms); })) {
                                filled = ::virtualcamera::FrameFiller::fillFromAHardwareBuffer(
                                        sHandleImporter, handle, width, height, src);
                            } else {
                                ALOGE("source fence wait failed; refusing CPU source read");
                            }
                            if (filled) {
                                mStats.cpuYuv++;
                                mStats.cpuYuvNs += std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count();
                            }
                        }
                        if (doneFence >= 0) {
                            // Same fence, two consumers: the framework must not
                            // read dst before it fires (release fence on the
                            // output buffer), and the producer must not
                            // overwrite src before it fires (fed back through
                            // the HAL as the buffer's release fence).
                            outputReleaseFence = ::dup(doneFence);
                        }
                        if (doneFence >= 0) source.fences.push_back(doneFence);
            }
#endif
        }

        StreamBuffer outBuffer;
        outBuffer.streamId = inBuffer.streamId;
        outBuffer.bufferId = inBuffer.bufferId;
        outBuffer.status = filled ? BufferStatus::OK : BufferStatus::ERROR;
        if (outputReleaseFence >= 0) {
            // Framework waits on this before consuming the buffer; ownership
            // of the fd moves into the parcel.
            outBuffer.releaseFence.fds.emplace_back(ndk::ScopedFileDescriptor(outputReleaseFence));
        }

        outputBuffers.push_back(std::move(outBuffer));
        mFrameCounter++;
    }

    // Log periodically, with the fill-path mix and per-path cost.
    if (mFrameCounter % 100 == 0) {
        auto ms = [](int64_t ns, uint64_t n) { return n ? (ns / 1e6) / n : 0.0; };
        ALOGI("Processed %d frames: gpu-rgba %llu, gpu-yuv %llu (%.1f ms), cpu-yuv %llu (%.1f ms), jpeg %llu (%.1f ms)",
              mFrameCounter.load(), (unsigned long long)mStats.gpuRgba,
              (unsigned long long)mStats.gpuYuv, ms(mStats.gpuYuvNs, mStats.gpuYuv),
              (unsigned long long)mStats.cpuYuv, ms(mStats.cpuYuvNs, mStats.cpuYuv),
              (unsigned long long)mStats.jpeg, ms(mStats.jpegNs, mStats.jpeg));
    }

    // A successful capture must contain at least one genuinely filled output.
    // Whole-request failure has no shutter/metadata; mixed results retain good
    // outputs and report ERROR_BUFFER only for the failed streams.
    const bool anyFilled = std::any_of(outputBuffers.begin(), outputBuffers.end(),
            [](const StreamBuffer& b) { return b.status == BufferStatus::OK; });
    if (!anyFilled) profile.fail(Profile::NoFilledOutput,Profile::Output);
    else profile.mark(Profile::Output);
    std::vector<NotifyMsg> notifications;
    if (anyFilled) {
        NotifyMsg shutter;
        shutter.set<NotifyMsg::Tag::shutter>(ShutterMsg{
            .frameNumber = request.frameNumber, .timestamp = timestamp,
            .readoutTimestamp = timestamp,
        });
        notifications.push_back(std::move(shutter));
        for (const auto& b : outputBuffers) {
            if (b.status != BufferStatus::ERROR) continue;
            NotifyMsg error;
            error.set<NotifyMsg::Tag::error>(ErrorMsg{
                .frameNumber = request.frameNumber, .errorStreamId = b.streamId,
                .errorCode = ErrorCode::ERROR_BUFFER,
            });
            notifications.push_back(std::move(error));
        }
    } else {
        NotifyMsg error;
        error.set<NotifyMsg::Tag::error>(ErrorMsg{
            .frameNumber = request.frameNumber, .errorStreamId = -1,
            .errorCode = ErrorCode::ERROR_REQUEST,
        });
        notifications.push_back(std::move(error));
    }
    mCallback->notify(notifications);

    // Build result metadata via core::MetadataBuilder
    CaptureResult captureResult;
    captureResult.frameNumber = request.frameNumber;
    captureResult.fmqResultSize = 0;
    captureResult.outputBuffers = std::move(outputBuffers);
    captureResult.inputBuffer.streamId = -1;
    captureResult.partialResult = anyFilled ? 1 : 0;
    captureResult.physicalCameraMetadata = {};
    if (anyFilled) captureResult.result.metadata = ::virtualcamera::MetadataBuilder::buildResultMetadata(
            timestamp, frameDurationNs, producerTs, mSourceMode.width, mSourceMode.height);

    std::vector<CaptureResult> results;
    results.push_back(std::move(captureResult));
    mCallback->processCaptureResult(results);
    profile.mark(Profile::Callback);

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
