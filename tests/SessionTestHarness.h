#pragma once
// Real Session/callback/gralloc tests. eventfd supplies controllable pollable
// dependencies, not GPU sync_files. Positive controls use actual producer
// submissions; no synthetic fallback is required by these tests.
#include "VirtualCameraSession.h"
#include "VirtualCameraStableHal.h"

#include <aidl/android/hardware/camera/device/BnCameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/BufferStatus.h>
#include <aidl/android/hardware/camera/device/ErrorCode.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android/sync.h>
#include <system/camera_metadata.h>
#include <vndk/hardware_buffer.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace device = aidl::android::hardware::camera::device;
using aidl::android::hardware::camera::provider::implementation::VirtualCameraSession;
using aidl::android::hardware::camera::provider::implementation::CameraStatus;
using aidl::android::hardware::camera::provider::implementation::VirtualCameraStableHal;

struct Producer {
    std::shared_ptr<VirtualCameraStableHal> hal;
    int slot;
    int64_t configurationToken = 0;
    std::vector<AHardwareBuffer*> buffers;
    Producer(std::shared_ptr<VirtualCameraStableHal> h, int s) : hal(std::move(h)), slot(s) {}
    ~Producer() { for (auto* b : buffers) AHardwareBuffer_release(b); }
    ndk::ScopedAStatus submit(int color = 255, int64_t timestamp = 1234567, int fence = -1,
                             bool reuse = false, bool expectAccepted = true) {
        if (!reuse) {
            AHardwareBuffer_Desc d{};
            d.width = 640; d.height = 480; d.layers = 1;
            d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
            d.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
            AHardwareBuffer* b = nullptr;
            assert(AHardwareBuffer_allocate(&d, &b) == 0 && b);
            AHardwareBuffer_describe(b, &d);
            void* data = nullptr;
            assert(AHardwareBuffer_lock(b, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &data) == 0);
            auto* pixels = static_cast<uint8_t*>(data);
            for (uint32_t y = 0; y < d.height; ++y) for (uint32_t x = 0; x < d.width; ++x) {
                auto* p = pixels + (y * d.stride + x) * 4;
                p[0] = p[1] = p[2] = color; p[3] = 255;
            }
            assert(AHardwareBuffer_unlock(b, nullptr) == 0);
            buffers.push_back(b);
        }
        assert(!buffers.empty());
        AHardwareBuffer_Desc d{};
        AHardwareBuffer_describe(buffers.back(), &d);
        auto handle = ::android::dupToAidl(AHardwareBuffer_getNativeHandle(buffers.back()));
        ndk::ScopedFileDescriptor acquire(fence < 0 ? -1 : ::dup(fence)), retired;
        assert(fence < 0 || acquire.get() >= 0);
        auto status = hal->queueFrameForConfiguration(slot, configurationToken, handle,
                d.width, d.height, d.stride, d.format, d.usage, timestamp, acquire, &retired);
        if (expectAccepted) assert(status.isOk());
        if (!status.isOk()) assert(retired.get() < 0);
        if (retired.get() >= 0) assert(sync_wait(retired.get(), 1000) == 0);
        return status;
    }
};

struct ReturnedBuffer {
    int id;
    int64_t bufferId;
    device::BufferStatus status;
    ndk::ScopedFileDescriptor fence;
};

struct Callback : device::BnCameraDeviceCallback {
    int shutters = 0, errors = 0, results = 0;
    bool emptyMetadata = false, cleanHandles = true;
    int partial = -1;
    int errorFrame = -1, resultFrame = -1;
    std::vector<int> events;
    std::vector<ReturnedBuffer> buffers;
    void reset() {
        shutters = errors = results = 0;
        emptyMetadata = false;
        cleanHandles = true;
        partial = -1;
        errorFrame = resultFrame = -1;
        events.clear();
        buffers.clear();
    }
    ndk::ScopedAStatus notify(const std::vector<device::NotifyMsg>& messages) override {
        for (const auto& message : messages) {
            if (message.getTag() == device::NotifyMsg::Tag::shutter) {
                ++shutters;
                events.push_back(1);
            } else {
                const auto& error = message.get<device::NotifyMsg::Tag::error>();
                assert(error.errorCode == device::ErrorCode::ERROR_REQUEST ||
                        error.errorCode == device::ErrorCode::ERROR_BUFFER);
                assert(error.errorCode != device::ErrorCode::ERROR_REQUEST || error.errorStreamId == -1);
                errorFrame = error.frameNumber;
                ++errors;
                events.push_back(2);
            }
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus processCaptureResult(const std::vector<device::CaptureResult>& captures) override {
        for (const auto& capture : captures) {
            ++results;
            resultFrame = capture.frameNumber;
            events.push_back(3);
            emptyMetadata = capture.result.metadata.empty();
            partial = capture.partialResult;
            for (const auto& buffer : capture.outputBuffers) {
                cleanHandles &= buffer.buffer.fds.empty() && buffer.buffer.ints.empty() &&
                        buffer.acquireFence.fds.empty() && buffer.acquireFence.ints.empty();
                assert(buffer.releaseFence.fds.size() <= 1 && buffer.releaseFence.ints.empty());
                int fd = buffer.releaseFence.fds.empty() ? -1 : ::dup(buffer.releaseFence.fds[0].get());
                assert(buffer.releaseFence.fds.empty() || fd >= 0);
                buffers.push_back({buffer.streamId, buffer.bufferId, buffer.status, ndk::ScopedFileDescriptor(fd)});
            }
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus requestStreamBuffers(const std::vector<device::BufferRequest>&,
            std::vector<device::StreamBufferRet>*, device::BufferRequestStatus*) override {
        assert(false); return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus returnStreamBuffers(const std::vector<device::StreamBuffer>&) override {
        assert(false); return ndk::ScopedAStatus::ok();
    }
};

struct Fixture {
    std::shared_ptr<Callback> callback = ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<VirtualCameraStableHal> hal;
    Producer producer;
    std::shared_ptr<VirtualCameraSession> session;
    std::vector<AHardwareBuffer*> targets;
    device::CameraMetadata settings;
    int frame = 0;
    int targetFormat;
    std::vector<int> targetFormats;
    uint64_t attemptsBefore = 0;
    explicit Fixture(int count, std::shared_ptr<VirtualCameraStableHal> stable = nullptr,
                     int slot = 0, int format = 0x23,
                     std::shared_ptr<const ::virtualcamera::ProducerCapabilities> capabilities = nullptr)
        : hal(stable ? std::move(stable) : ndk::SharedRefBase::make<VirtualCameraStableHal>()),
          producer(hal, slot),
          session(ndk::SharedRefBase::make<VirtualCameraSession>(
                  callback, slot, nullptr, nullptr, nullptr, hal, capabilities)), targetFormat(format) {
        device::StreamConfiguration configuration;
        for (int i = 0; i < count; ++i) {
            // Three-output tests use the advertised two processed + one JPEG
            // contract; three processed outputs are intentionally unsupported.
            const int outputFormat = count == 3 && i == 2 ? 0x21 : format;
            targetFormats.push_back(outputFormat);
            AHardwareBuffer_Desc description{};
            description.width = 640; description.height = 480; description.layers = 1;
            description.format = outputFormat;
            if (outputFormat == 0x21) { description.width = 2 * 1024 * 1024; description.height = 1; }
            description.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                    AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
            if (outputFormat == 1) description.usage |= AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
            AHardwareBuffer* target = nullptr;
            assert(AHardwareBuffer_allocate(&description, &target) == 0 && target);
            targets.push_back(target);
            device::Stream stream;
            stream.id = i + 1; stream.width = 640; stream.height = 480;
            stream.format = static_cast<decltype(stream.format)>(outputFormat);
            if (outputFormat == 0x21) stream.bufferSize = description.width;
            configuration.streams.push_back(stream);
        }
        std::vector<device::HalStream> streams;
        assert(session->configureStreams(configuration, &streams).isOk());
        producer.configurationToken = session->sourceConfigurationToken();
        assert(producer.configurationToken > 0);
        assert(session->constructDefaultRequestSettings(device::RequestTemplate::PREVIEW, &settings).isOk());
        pixels(true);
    }
    ~Fixture() {
        session->close();
        session.reset();
        for (auto* target : targets) AHardwareBuffer_release(target);
    }
    std::vector<uint8_t> pixels(bool initialize = false) {
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i < targets.size(); ++i) {
            auto* target = targets[i];
            const int targetFormat = targetFormats[i];
            if (targetFormat != 0x23) {
                AHardwareBuffer_Desc d{};
                AHardwareBuffer_describe(target, &d);
                void* data = nullptr;
                assert(AHardwareBuffer_lock(target, initialize ? AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN :
                        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &data) == 0);
                const int pixelSize = targetFormat == 1 ? 4 : 1;
                auto* base = static_cast<uint8_t*>(data);
                for (uint32_t y = 0; y < d.height; ++y) for (uint32_t x = 0; x < d.width * pixelSize; ++x) {
                    auto& value = base[y * d.stride * pixelSize + x];
                    if (initialize) value = 0x5a;
                    bytes.push_back(value);
                }
                assert(AHardwareBuffer_unlock(target, nullptr) == 0);
                continue;
            }
            AHardwareBuffer_Planes planes{};
            assert(AHardwareBuffer_lockPlanes(target, initialize ? AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN :
                    AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &planes) == 0);
            assert(planes.planeCount == 3);
            for (uint32_t p = 0; p < planes.planeCount; ++p) {
                int width = p == 0 ? 640 : 320, height = p == 0 ? 480 : 240;
                auto& plane = planes.planes[p];
                auto* base = static_cast<uint8_t*>(plane.data);
                for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                    auto& value = base[y * plane.rowStride + x * plane.pixelStride];
                    if (initialize) value = 0x5a;
                    bytes.push_back(value);
                }
            }
            assert(AHardwareBuffer_unlock(target, nullptr) == 0);
        }
        return bytes;
    }
    void fps(int32_t value) {
        auto* metadata = reinterpret_cast<camera_metadata_t*>(settings.metadata.data());
        camera_metadata_entry_t entry;
        assert(find_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, &entry) == 0);
        int32_t range[] = {value, value};
        assert(update_camera_metadata_entry(metadata, entry.index, range, 2, nullptr) == 0);
        // Drive the real rate transition before tests submit the new source.
        // A rate change retires the previous source token by design.
        int32_t accepted = -1;
        assert(run(request(std::vector<int>(targets.size(), -1)), &accepted).isOk());
        assert(accepted == 1);
        producer.configurationToken = session->sourceConfigurationToken();
        callback->reset();
    }
    device::CaptureRequest request(const std::vector<int>& fences) {
        assert(fences.size() == targets.size());
        device::CaptureRequest request;
        request.frameNumber = ++frame;
        request.inputBuffer.streamId = -1;
        request.settings = settings;
        for (size_t i = 0; i < targets.size(); ++i) {
            device::StreamBuffer buffer;
            buffer.streamId = i + 1; buffer.bufferId = i + 1;
            buffer.buffer = ::android::dupToAidl(AHardwareBuffer_getNativeHandle(targets[i]));
            if (fences[i] >= 0) {
                int fd = ::dup(fences[i]); assert(fd >= 0);
                buffer.acquireFence.fds.emplace_back(fd);
            }
            request.outputBuffers.push_back(std::move(buffer));
        }
        return request;
    }
    ndk::ScopedAStatus run(device::CaptureRequest request, int32_t* accepted) {
        attemptsBefore = session->outputRenderAttempts();
        std::vector<device::CaptureRequest> requests;
        requests.push_back(std::move(request));
        return session->processCaptureRequest(requests, {}, accepted);
    }
    bool failedUntouched(const std::vector<uint8_t>& before, int32_t accepted) {
        if (accepted != 1 || callback->shutters != 0 || callback->errors != 1 ||
                callback->results != 1 || !callback->emptyMetadata || callback->partial != 0 ||
                !callback->cleanHandles || callback->events != std::vector<int>({2, 3}) ||
                callback->buffers.size() != targets.size() || pixels() != before ||
                session->outputRenderAttempts() != attemptsBefore ||
                callback->errorFrame != frame || callback->resultFrame != frame) return false;
        for (size_t i = 0; i < callback->buffers.size(); ++i) {
            const auto& buffer = callback->buffers[i];
            if (buffer.status != device::BufferStatus::ERROR || buffer.id != static_cast<int>(i + 1) ||
                    buffer.bufferId != static_cast<int64_t>(i + 1)) return false;
        }
        return true;
    }
};
