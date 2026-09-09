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
#include <android/sync.h>
#include <android-base/properties.h>
#include <hardware/gralloc.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

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
        const std::shared_ptr<ICameraDeviceCallback>& callback,
        std::shared_ptr<::virtualcamera::VirtualCameraFrameSource> frameSource,
        std::shared_ptr<::virtualcamera::VirtualCameraFrameSourceV2> frameSourceV2,
        std::shared_ptr<::virtualcamera::AidlFrameSource> aidlSource)
    : mCallback(callback),
      mFrameSource(frameSource),
      mFrameSourceV2(frameSourceV2),
      mAidlSource(aidlSource) {
    mForceCpuYuv = ::android::base::GetBoolProperty("vendor.vcam.yuv.cpu", false);
    ALOGI("VirtualCameraSession created (AIDL V1 adapter, v1 + v2 frame sources)%s",
          mForceCpuYuv ? " [vendor.vcam.yuv.cpu=1: CPU YUV converter forced]" : "");
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
#ifdef VCAM_AIDL_SOURCE
    if (mAidlSource && !mClosed) {
        mAidlSource->sessionClosed();
    }
#endif
#ifdef VCAM_STABLE_AIDL
    if (!mClosed) {
        if (auto* hal = VirtualCameraStableHal::get()) hal->notifyCameraClosed();
    }
#endif
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
    mHaveSettings = false;
    for (auto& pair : mBufferCache) {
        if (pair.second != nullptr) {
            sHandleImporter.freeBuffer(pair.second);
        }
    }
    mBufferCache.clear();
    halStreams->clear();

    ALOGI("Configuring %zu streams", requestedConfiguration.streams.size());

    // Same rule as ICameraDevice.isStreamCombinationSupported: only advertised
    // formats/sizes, no input streams, no stream use cases (none advertised).
    {
        std::vector<::virtualcamera::MetadataBuilder::StreamDesc> descs;
        for (const auto& s : requestedConfiguration.streams) {
            descs.push_back({static_cast<int>(s.format), s.width, s.height,
                             static_cast<int64_t>(s.useCase),
                             s.streamType == ::aidl::android::hardware::camera::device::StreamType::INPUT,
                             static_cast<int>(s.rotation)});
        }
        if (!::virtualcamera::MetadataBuilder::isStreamCombinationSupported(descs)) {
            ALOGE("configureStreams: unsupported stream combination");
            return ndk::ScopedAStatus::fromServiceSpecificError(
                    static_cast<int32_t>(CameraStatus::ILLEGAL_ARGUMENT));
        }
    }

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
        const auto& primary = requestedConfiguration.streams[0];
        mAidlSource->configureStreams(primary.width, primary.height, 30);
    }
#endif
#ifdef VCAM_STABLE_AIDL
    // Stable-AIDL relay: tell the platform service the stream shape so it can
    // create the BufferQueue and hand its Surface to the producer app.
    if (!requestedConfiguration.streams.empty()) {
        const auto& primary = requestedConfiguration.streams[0];
        if (auto* hal = VirtualCameraStableHal::get()) {
            hal->notifyStreamsConfigured(primary.width, primary.height, 30);
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
    metadata->metadata = ::virtualcamera::MetadataBuilder::buildDefaultRequestSettings();
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
            if (mStreams.find(b.streamId) == mStreams.end()) valid = false;
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
        CameraStatus status = processSingleRequest(request);
        if (status != CameraStatus::OK) {
            ALOGE("Failed to process request %d", request.frameNumber);
            return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(status));
        }
        (*numRequestsProcessed)++;
    }

    return ndk::ScopedAStatus::ok();
}

void VirtualCameraSession::updateTargetFps(const CameraMetadata& settings) {
    if (settings.metadata.empty()) return;   // unchanged since the last request
    mHaveSettings = true;
    const auto* meta = reinterpret_cast<const camera_metadata_t*>(settings.metadata.data());
    const size_t size = settings.metadata.size();
    if (validate_camera_metadata_structure(meta, &size) != 0) return;
    camera_metadata_ro_entry_t e;
    if (find_camera_metadata_ro_entry(meta, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, &e) != 0 ||
        e.count < 2) {
        return;
    }
    int32_t fps = e.data.i32[1];   // upper bound of the requested range
    if (fps > 0 && fps <= 120 && fps != mTargetFps) {
        ALOGI("Target fps %d -> %d (AE_TARGET_FPS_RANGE [%d,%d])",
              mTargetFps, fps, e.data.i32[0], e.data.i32[1]);
        mTargetFps = fps;
    }
    if (find_camera_metadata_ro_entry(meta, ANDROID_JPEG_QUALITY, &e) == 0 && e.count >= 1) {
        mJpegQuality = e.data.u8[0];
    }
}

int64_t VirtualCameraSession::paceFrame() {
    using namespace std::chrono;
    const int64_t interval = 1000000000LL / mTargetFps;
    int64_t now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    if (mNextFrameNs == 0 || now > mNextFrameNs + interval) {
        mNextFrameNs = now;   // first frame, or we fell more than a slot behind: resync
    }
    if (now < mNextFrameNs) {
        std::this_thread::sleep_for(nanoseconds(mNextFrameNs - now));
    }
    const int64_t slot = mNextFrameNs;
    mNextFrameNs += interval;
    return slot;
}

CameraStatus VirtualCameraSession::processSingleRequest(const CaptureRequest& request) {
    // Pace to the requested frame rate: without this the loop free-runs at
    // whatever rate the framework can cycle buffers (thousands of fps on a
    // fast GPU), which is wrong for consumers and burns power. The paced
    // slot is the frame's sensor timestamp.
    updateTargetFps(request.settings);
    int64_t timestamp = paceFrame();
    const int64_t frameDurationNs = 1000000000LL / mTargetFps;
    int64_t producerTs = 0;   // stable-AIDL path: producer's queue timestamp of the frame used

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
        int outputReleaseFence = -1;   // owned; attached to the result buffer

        // Import/cache the buffer (adapter owns this — touches AIDL StreamBuffer)
        buffer_handle_t handle = importBuffer(inBuffer);

        // Get stream info for dimensions
        auto streamIt = mStreams.find(inBuffer.streamId);
        if (streamIt != mStreams.end() && handle != nullptr) {
            int width = streamIt->second.width;
            int height = streamIt->second.height;

            // Delegate frame filling to core — try v2 zero-copy first, fall back to v1
            bool filled = false;
#ifdef VCAM_AIDL_SOURCE
            if (mAidlSource) {
                mAidlSource->retryIfNeeded();
                filled = mAidlSource->fillLatestInto(sHandleImporter, handle,
                                                     width, height);
            }
#endif
#ifdef VCAM_STABLE_AIDL
            if (!filled) {
                if (auto* hal = VirtualCameraStableHal::get()) {
                    int64_t srcTs = 0;
                    int acquireFence = -1;   // producer's GPU-done fence (ours to close)
                    if (AHardwareBuffer* src = hal->acquireLatest(&srcTs, &acquireFence)) {
                        producerTs = srcTs;
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
                            filled = ::virtualcamera::GpuCompositor::get().composite(
                                    src, acquireFence, handle, width, height,
                                    /*stride*/ width, dstFormat,
                                    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                                    AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
                                    &doneFence);
                            if (filled) mStats.gpuRgba++;
                        }
#endif
                        if (!filled && dstFormat != kFmtBlob) {
                            // CPU converter reads src directly: wait for the
                            // producer's render to land first.
                            const auto t0 = clk::now();
                            if (acquireFence >= 0) sync_wait(acquireFence, 100);
                            filled = ::virtualcamera::FrameFiller::fillFromAHardwareBuffer(
                                    sHandleImporter, handle, width, height, src);
                            if (filled) {
                                mStats.cpuYuv++;
                                mStats.cpuYuvNs += std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count();
                            }
                        }
                        if (acquireFence >= 0) ::close(acquireFence);
                        if (doneFence >= 0) {
                            // Same fence, two consumers: the framework must not
                            // read dst before it fires (release fence on the
                            // output buffer), and the producer must not
                            // overwrite src before it fires (fed back through
                            // the HAL as the buffer's release fence).
                            outputReleaseFence = ::dup(doneFence);
                        }
                        hal->releaseFrame(src, doneFence);   // takes doneFence
                    }
                }
            }
#endif
            if (!filled && mFrameSourceV2 && mFrameSourceV2->isActive()) {
                filled = ::virtualcamera::FrameFiller::fillBufferFromV2(
                    sHandleImporter, handle, width, height,
                    mFrameSourceV2.get());
            }
            if (!filled) {
                ::virtualcamera::FrameFiller::fillYuvBufferFromRenderer(
                    sHandleImporter, handle, width, height,
                    mFrameCounter.load(), mFrameSource.get());
            }
        }

        StreamBuffer outBuffer;
        outBuffer.streamId = inBuffer.streamId;
        outBuffer.bufferId = inBuffer.bufferId;
        outBuffer.status = BufferStatus::OK;
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

    // Build result metadata via core::MetadataBuilder
    CaptureResult captureResult;
    captureResult.frameNumber = request.frameNumber;
    captureResult.fmqResultSize = 0;
    captureResult.outputBuffers = std::move(outputBuffers);
    captureResult.inputBuffer.streamId = -1;
    captureResult.partialResult = 1;
    captureResult.physicalCameraMetadata = {};
    captureResult.result.metadata = ::virtualcamera::MetadataBuilder::buildResultMetadata(
            timestamp, frameDurationNs, producerTs);

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
