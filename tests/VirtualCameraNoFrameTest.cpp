#include "SessionTestHarness.h"
#include <cstring>
#include <aidl/android/hardware/virtualcamera/hal/BnVirtualCameraHalCallback.h>

static void success(Fixture& f, int32_t accepted) {
    assert(accepted == 1 && f.callback->shutters == 1 && f.callback->errors == 0);
    assert(f.callback->results == 1 && !f.callback->emptyMetadata && f.callback->partial == 1);
    for (const auto& b : f.callback->buffers) {
        assert(b.status == device::BufferStatus::OK);
        if (b.fence.get() >= 0) assert(sync_wait(b.fence.get(), 1000) == 0);
    }
}

// Opt-in red regression: force the ordering seen by the end-to-end pixel probe.
// Real HAL/Session/gralloc, direct calls; not JVM/Binder/GPU race injection.
static int lateOldSessionFrame() {
    for (bool lateSubmission : {false, true}) {
        auto hal = ndk::SharedRefBase::make<VirtualCameraStableHal>();
        Fixture old(1, hal);
        old.producer.submit(32);
        int32_t accepted = -1;
        assert(old.run(old.request({-1}), &accepted).isOk());
        success(old, accepted);
        assert(old.session->close().isOk());

        if (lateSubmission) {
            // Represents an old relay RPC that arrives after close drops the
            // latest frame, but before the old queue's async teardown finishes.
            // The current V3 frame API has no originating-session identity.
            auto late = old.producer.submit(224, 1234567, -1, false, false);
            if (late.isOk()) {
                std::fprintf(stderr, "FAIL: closed configuration accepted late source before reopen\n");
                return 1;
            }
            assert(late.getServiceSpecificError() == 7);
        }
        Fixture next(1, hal);
        const auto before = next.pixels();
        accepted = -1;
        assert(next.run(next.request({-1}), &accepted).isOk());
        if (!next.failedUntouched(before, accepted)) {
            std::fprintf(stderr,
                    "FAIL: new session consumed old-session late submission: late=%d shutters=%d errors=%d writes=%llu changedPixels=%d\n",
                    lateSubmission, next.callback->shutters, next.callback->errors,
                    static_cast<unsigned long long>(next.session->outputRenderAttempts() - next.attemptsBefore),
                    next.pixels() != before);
            return 1;
        }
        std::printf("PASS: new session rejects old source (late=%d)\n", lateSubmission);
        next.callback->reset();
        next.producer.submit(128);
        assert(next.run(next.request({-1}), &accepted).isOk());
        success(next, accepted); // reject stale input without disabling valid new work
    }
    return 0;
}

static int configurationAdmissionCases() {
    int cases = 0;
    {
        Fixture f(1); f.producer.submit(64);
        AHardwareBuffer_Desc description{};
        AHardwareBuffer_describe(f.producer.buffers.back(), &description);
        auto handle = ::android::dupToAidl(AHardwareBuffer_getNativeHandle(f.producer.buffers.back()));
        ndk::ScopedFileDescriptor noFence, retired;
        auto v1 = f.hal->queueFrame(handle, description.width, description.height,
                description.stride, description.format, description.usage, 1);
        auto v2 = f.hal->queueFrameFenced(handle, description.width, description.height,
                description.stride, description.format, description.usage, 1, noFence, &retired);
        auto v3 = f.hal->queueFrameForCamera(0, handle, description.width, description.height,
                description.stride, description.format, description.usage, 1, noFence, &retired);
        assert(!v1.isOk() && !v2.isOk() && !v3.isOk() && retired.get() < 0);
        assert(v1.getServiceSpecificError() == 7 && v2.getServiceSpecificError() == 7 && v3.getServiceSpecificError() == 7);
        int32_t accepted = -1;
        assert(f.run(f.request({-1}), &accepted).isOk()); success(f, accepted);
        ++cases;
    }
    {
        auto hal = ndk::SharedRefBase::make<VirtualCameraStableHal>();
        Fixture old(1, hal), other(1, hal, 1);
        old.producer.submit(32);
        Producer wrongSlot(hal, 1);
        wrongSlot.configurationToken = old.producer.configurationToken;
        assert(!wrongSlot.submit(64, 1, -1, false, false).isOk());
        wrongSlot.configurationToken = 0;
        assert(!wrongSlot.submit(64, 1, -1, false, false).isOk());
        ++cases;
        Fixture replacement(1, hal);
        replacement.producer.submit(128);
        assert(old.session->close().isOk()); // old close cannot retire replacement
        assert(!old.producer.submit(224, 2, -1, false, false).isOk());
        int32_t accepted = -1;
        assert(replacement.run(replacement.request({-1}), &accepted).isOk());
        success(replacement, accepted);
        ++cases;
        other.producer.submit(64);
        assert(other.run(other.request({-1}), &accepted).isOk());
        success(other, accepted);
        ++cases;
    }
    {
        Fixture f(1);
        f.producer.submit(64);
        int64_t oldToken = f.producer.configurationToken;
        device::StreamConfiguration configuration;
        device::Stream stream;
        stream.id = 1; stream.width = 640; stream.height = 480;
        stream.format = static_cast<decltype(stream.format)>(0x23);
        configuration.streams.push_back(stream);
        std::vector<device::HalStream> configured;
        assert(f.session->configureStreams(configuration, &configured).isOk());
        assert(f.session->sourceConfigurationToken() > oldToken);
        assert(!f.producer.submit(224, 3, -1, false, false).isOk());
        f.producer.configurationToken = f.session->sourceConfigurationToken();
        f.producer.submit(128);
        int32_t accepted = -1;
        assert(f.run(f.request({-1}), &accepted).isOk()); success(f, accepted);
        ++cases;
    }
    {
        Fixture f(1); f.producer.submit(64);
        int64_t timestamp = 0; int fence = -1;
        AHardwareBuffer* reader = f.hal->acquireLatest(0, &timestamp, &fence, nullptr, 0,
                f.producer.configurationToken);
        assert(reader && fence == -1);
        std::atomic<bool> entered{false}, rejected{false};
        std::thread submit([&] {
            entered = true;
            auto status = f.producer.submit(224, 4, -1, false, false);
            rejected = !status.isOk() && status.getServiceSpecificError() == 7;
        });
        while (!entered) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        f.session->close();
        f.hal->releaseFrame(0, reader, -1);
        submit.join(); assert(rejected);
        ++cases;
    }
    std::printf("PASS: %d configuration admission/ownership/reconfigure/wait cases\n", cases);
    return 0;
}

namespace vneg = aidl::android::hardware::virtualcamera::hal;
struct NegotiationCallback : vneg::BnVirtualCameraHalCallback {
    int64_t token=0;int fps=0;
    ndk::ScopedAStatus onStreamsConfiguredForProducer(int32_t slot,int64_t,int64_t t,int32_t w,int32_t h,int32_t f) override{return onStreamsConfiguredForConfiguration(slot,t,w,h,f);}
    ndk::ScopedAStatus onCameraClosedForProducer(int32_t slot,int64_t,int64_t t) override{return onCameraClosedForConfiguration(slot,t);}
    ndk::ScopedAStatus onStreamsConfigured(int32_t,int32_t,int32_t) override{return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onCameraClosed() override{return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onStreamsConfiguredForCamera(int32_t,int32_t,int32_t,int32_t) override{return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onCameraClosedForCamera(int32_t) override{return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onStreamsConfiguredForConfiguration(int32_t,int64_t t,int32_t,int32_t,int32_t f) override{token=t;fps=f;return ndk::ScopedAStatus::ok();}
    ndk::ScopedAStatus onCameraClosedForConfiguration(int32_t,int64_t) override{return ndk::ScopedAStatus::ok();}
};
static int producerNegotiationCases() {
    auto hal=ndk::SharedRefBase::make<VirtualCameraStableHal>();
    auto owner=ndk::SharedRefBase::make<NegotiationCallback>();
    auto stranger=ndk::SharedRefBase::make<NegotiationCallback>();
    assert(hal->setCallback(owner).isOk());
    vneg::ProducerDescriptor d;d.registration=43;
    vneg::ProducerMode mode;mode.id=1;mode.width=640;mode.height=480;mode.minFps=15;mode.maxFps=60;mode.outputFormats={1,35};
    d.modes={mode};
    assert(!hal->registerProducer(0,d,stranger).isOk());
    auto bad=d;bad.modes[0].maxFps=121;assert(!hal->registerProducer(0,bad,owner).isOk());assert(!hal->capabilities(0));
    assert(hal->registerProducer(0,d,owner).isOk());
    assert(!hal->registerProducer(0,d,owner).isOk());
    assert(!hal->removeProducer(0,999,owner).isOk());
    assert(!hal->removeProducer(0,43,stranger).isOk());
    Fixture f(1,hal,0,0x23,hal->capabilities(0));
    const int64_t old=f.producer.configurationToken;
    assert(!f.producer.submit(32,1,-1,false,false).isOk());
    assert(!hal->setConfigurationReady(0,old+100,true).isOk());
    assert(hal->setConfigurationReady(0,old,true).isOk());
    f.producer.submit(64);int32_t accepted=-1;
    assert(f.run(f.request({-1}),&accepted).isOk());success(f,accepted);
    f.callback->reset();
    auto* meta=allocate_camera_metadata(10,200);int32_t range[]={60,60};
    add_camera_metadata_entry(meta,ANDROID_CONTROL_AE_TARGET_FPS_RANGE,range,2);
    auto bytes=get_camera_metadata_size(meta);f.settings.metadata.resize(bytes);std::memcpy(f.settings.metadata.data(),meta,bytes);free_camera_metadata(meta);
    auto before=f.pixels();assert(f.run(f.request({-1}),&accepted).isOk());assert(f.failedUntouched(before,accepted));
    const int64_t next=f.session->sourceConfigurationToken();assert(next>old&&owner->token==next&&owner->fps==60);
    assert(!hal->setConfigurationReady(0,old,true).isOk());
    assert(!f.producer.submit(128,2,-1,false,false).isOk());
    f.producer.configurationToken=next;assert(hal->setConfigurationReady(0,next,true).isOk());
    f.producer.submit(128);f.callback->reset();assert(f.run(f.request({-1}),&accepted).isOk());success(f,accepted);
    assert(hal->setConfigurationReady(0,next,false).isOk());
    assert(!hal->setConfigurationReady(0,next,true).isOk()); // rejection permanently closes this token
    assert(!f.producer.submit(224,3,-1,false,false).isOk());
    assert(hal->removeProducer(0,43,owner).isOk());assert(!hal->capabilities(0));
    d.registration=44;d.modes[0].maxFps=30;assert(hal->registerProducer(0,d,owner).isOk());
    assert(!f.run(f.request({-1}),&accepted).isOk()); // old registration cannot negotiate replacement
    Fixture replacement(1,hal,0,0x23,hal->capabilities(0));
    f.session->close();assert(hal->setConfigurationReady(0,replacement.producer.configurationToken,true).isOk());
    replacement.producer.submit(192);replacement.callback->reset();assert(replacement.run(replacement.request({-1}),&accepted).isOk());success(replacement,accepted);
    assert(hal->setCallback(stranger).isOk());assert(!hal->capabilities(0));
    assert(!hal->registerProducer(0,d,owner).isOk());assert(hal->registerProducer(0,d,stranger).isOk());
    std::puts("PASS: descriptor validation/owner/immutability, ready gate, FPS token rollover, reject, replacement and callback lease cases");
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--reopen-late-frame") == 0) return lateOldSessionFrame();
    if (argc == 2 && std::strcmp(argv[1], "--configuration-admission") == 0) return configurationAdmissionCases();
    if (argc == 2 && std::strcmp(argv[1], "--producer-negotiation") == 0) return producerNegotiationCases();
    assert(argc == 1);
    int cases = 0;
    for (int format : {0x23, 1, 0x21}) {
    Fixture f(1, nullptr, 0, format);
    const auto before = f.pixels();
    int32_t accepted = -1;
    assert(f.run(f.request({-1}), &accepted).isOk());
    if (!f.failedUntouched(before, accepted)) {
        std::fprintf(stderr, "FAIL: missing producer frame wrote output or reported success (shutters=%d errors=%d)\n",
                f.callback->shutters, f.callback->errors);
        return 1;
    }
    ++cases;
    f.callback->reset();
    f.producer.submit();
    assert(f.run(f.request({-1}), &accepted).isOk());
    success(f, accepted);
    const auto produced = f.pixels(); assert(produced != before);
    f.callback->reset();
    auto cached = f.request({-1});
    cached.settings.metadata.clear(); cached.outputBuffers[0].buffer = {};
    assert(f.run(std::move(cached), &accepted).isOk());
    assert(f.failedUntouched(produced, accepted));
    ++cases;
    f.callback->reset();
    // Same buffer AND same timestamp, but an explicit new submission is fresh.
    f.producer.submit(255, 1234567, -1, true);
    assert(f.run(f.request({-1}), &accepted).isOk());
    success(f, accepted);
    ++cases;
    }
    {
        Fixture f(3); f.producer.submit(0);
        const auto before = f.pixels(); int32_t accepted = -1;
        assert(f.run(f.request({-1, -1, -1}), &accepted).isOk());
        success(f, accepted);
        const auto output = f.pixels(); assert(output != before);
        const size_t size = 640 * 480 * 3 / 2;
        assert(std::equal(output.begin(), output.begin() + size, output.begin() + size));
        // The third output is JPEG, so only the two YUV outputs match bytewise.
        f.callback->reset();
        assert(f.run(f.request({-1, -1, -1}), &accepted).isOk());
        assert(f.failedUntouched(output, accepted));
        ++cases;
    }
    {
        Fixture f(1); const auto before = f.pixels();
        f.fps(15); // lowest declared diagnostic cadence; transition is primed before source submission
        std::thread producer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            assert(f.pixels() == before);
            f.producer.submit();
        });
        int32_t accepted = -1;
        assert(f.run(f.request({-1}), &accepted).isOk());
        producer.join(); success(f, accepted);
        ++cases;
    }
    {
        Fixture f(1); const auto before = f.pixels();
        // Cold start may exceed 33ms without changing the steady-state budget.
        std::thread producer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            assert(f.pixels() == before);
            f.producer.submit();
        });
        int32_t accepted = -1;
        assert(f.run(f.request({-1}), &accepted).isOk());
        producer.join(); success(f, accepted);
        ++cases;
    }
    for (bool signal : {false, true}) {
        Fixture f(1); f.fps(15); const auto before = f.pixels();
        ndk::ScopedFileDescriptor fence(eventfd(0, EFD_CLOEXEC)); assert(fence.get() >= 0);
        f.producer.submit(255, 123, fence.get());
        std::thread producer;
        if (signal) producer = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            assert(f.pixels() == before);
            assert(eventfd_write(fence.get(), 1) == 0);
        });
        int32_t accepted = -1;
        assert(f.run(f.request({-1}), &accepted).isOk());
        if (signal) { producer.join(); success(f, accepted); }
        else {
            assert(f.failedUntouched(before, accepted));
            assert(eventfd_write(fence.get(), 1) == 0);
            f.callback->reset();
            assert(f.run(f.request({-1}), &accepted).isOk());
            assert(f.failedUntouched(before, accepted)); // rejected source cannot replay
        }
        ++cases;
    }
    {
        Fixture f(1); f.fps(15); const auto before = f.pixels();
        ndk::ScopedFileDescriptor fence(eventfd(0, EFD_CLOEXEC)); assert(fence.get() >= 0);
        f.producer.submit(255, 456, fence.get());
        std::thread removal([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            assert(f.hal->setCameraPresent(0, false).isOk());
            assert(eventfd_write(fence.get(), 1) == 0);
        });
        int32_t accepted = -1;
        assert(f.run(f.request({-1}), &accepted).isOk());
        removal.join();
        assert(f.failedUntouched(before, accepted));
        ++cases;
    }
    for (bool close : {false, true}) {
        Fixture f(1); f.fps(15); const auto before = f.pixels();
        int32_t accepted = -1;
        auto request = f.request({-1});
        std::atomic<bool> started{false}, done{false};
        std::thread capture([&] {
            started = true;
            assert(f.run(std::move(request), &accepted).isOk());
            done = true;
        });
        while (!started) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(!done);
        const auto begin = std::chrono::steady_clock::now();
        assert((close ? f.session->close() : f.session->flush()).isOk());
        capture.join();
        assert(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(250));
        assert(f.failedUntouched(before, accepted));
        if (!close) {
            f.callback->reset(); f.fps(30); f.producer.submit();
            assert(f.run(f.request({-1}), &accepted).isOk());
            success(f, accepted);
        }
        ++cases;
    }
    {
        auto hal = ndk::SharedRefBase::make<VirtualCameraStableHal>();
        Fixture a(1, hal, 0), b(1, hal, 1);
        a.producer.submit(255); b.producer.submit(0);
        int32_t accepted = -1;
        assert(a.run(a.request({-1}), &accepted).isOk()); success(a, accepted);
        assert(b.run(b.request({-1}), &accepted).isOk()); success(b, accepted);
        assert(a.pixels() != b.pixels());
        const auto before = a.pixels(); a.callback->reset();
        assert(a.run(a.request({-1}), &accepted).isOk()); assert(a.failedUntouched(before, accepted));
        b.callback->reset(); b.producer.submit(255);
        assert(b.run(b.request({-1}), &accepted).isOk()); success(b, accepted);
        // Removal invalidates the old session; reappearance needs a new session/configuration.
        b.callback->reset(); assert(hal->setCameraPresent(1, false).isOk());
        const auto removed = b.pixels();
        assert(b.run(b.request({-1}), &accepted).isOk()); assert(b.failedUntouched(removed, accepted));
        assert(hal->setCameraPresent(1, true).isOk());
        assert(!b.producer.submit(0, 1234567, -1, false, false).isOk());
        Fixture reappeared(1, hal, 1);
        reappeared.producer.submit(0);
        assert(reappeared.run(reappeared.request({-1}), &accepted).isOk()); success(reappeared, accepted);
        ++cases;
    }
    std::printf("PASS: %d no-frame/freshness/cancellation/format/isolation cases\n", cases);
}
