// Compile the production JNI relay in this isolated native process. Real
// BufferQueues/gralloc + a controlled local HAL; no JVM or GPU work submitted.
#include "../platform-jni/VirtualCameraRelayJni.cpp"
#include <aidl/android/hardware/virtualcamera/hal/BnVirtualCameraHal.h>
#include <gui/Surface.h>
#include <android/native_window.h>
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>
#include <cstdio>

struct FakeHal : vhal::BnVirtualCameraHal {
    std::mutex lock;
    std::condition_variable cv;
    bool block = false, entered = false;
    int blockedSlot = 0;
    std::atomic<int> calls{0}, presence{0};
    std::atomic<int> failure{0};
    std::atomic<int64_t> tokenSeen{0};
    ndk::ScopedAStatus registerProducer(int32_t,const vhal::ProducerDescriptor&,const std::shared_ptr<vhal::IVirtualCameraHalCallback>&) override { return ndk::ScopedAStatus::fromServiceSpecificError(8); }
    ndk::ScopedAStatus removeProducer(int32_t,int64_t,const std::shared_ptr<vhal::IVirtualCameraHalCallback>&) override { return ndk::ScopedAStatus::fromServiceSpecificError(8); }
    ndk::ScopedAStatus setConfigurationReady(int32_t,int64_t,bool) override { return ndk::ScopedAStatus::fromServiceSpecificError(8); }
    ndk::ScopedAStatus queueFrameForConfiguration(int32_t slot, int64_t token,
            const ::aidl::android::hardware::common::NativeHandle& handle,
            int32_t w, int32_t h, int32_t stride, int32_t format, int64_t usage, int64_t timestamp,
            const ndk::ScopedFileDescriptor& acquire, ndk::ScopedFileDescriptor* retired) override {
        tokenSeen = token;
        return queueFrameForCamera(slot, handle, w, h, stride, format, usage, timestamp, acquire, retired);
    }
    ndk::ScopedAStatus setCallback(const std::shared_ptr<vhal::IVirtualCameraHalCallback>&) override {
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus setProducerAvailable(bool) override { ++presence; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setCameraPresent(int32_t, bool) override { ++presence; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus getMaxCameras(int32_t* n) override { *n = kMaxSlots; return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus queueFrame(const ::aidl::android::hardware::common::NativeHandle&, int32_t, int32_t,
            int32_t, int32_t, int64_t, int64_t) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus queueFrameFenced(const ::aidl::android::hardware::common::NativeHandle&,
            int32_t, int32_t, int32_t, int32_t, int64_t, int64_t,
            const ndk::ScopedFileDescriptor&, ndk::ScopedFileDescriptor*) override {
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus queueFrameForCamera(int32_t slot,
            const ::aidl::android::hardware::common::NativeHandle&,
            int32_t, int32_t, int32_t, int32_t, int64_t, int64_t,
            const ndk::ScopedFileDescriptor&, ndk::ScopedFileDescriptor* retired) override {
        ++calls; retired->set(-1);
        if (failure == 1) return ndk::ScopedAStatus::fromServiceSpecificError(6);
        if (failure == 2) return ndk::ScopedAStatus::fromStatus(STATUS_DEAD_OBJECT);
        std::unique_lock<std::mutex> guard(lock);
        if (block && slot == blockedSlot) {
            entered = true; cv.notify_all();
            assert(cv.wait_for(guard, std::chrono::seconds(2), [&] { return !block; }));
        }
        return ndk::ScopedAStatus::ok();
    }
    void pause() { std::lock_guard<std::mutex> guard(lock); block = true; entered = false; }
    void waitEntered() {
        std::unique_lock<std::mutex> guard(lock);
        assert(cv.wait_for(guard, std::chrono::seconds(1), [&] { return entered; }));
    }
    void resume() { std::lock_guard<std::mutex> guard(lock); block = false; cv.notify_all(); }
};

static void connect(const std::shared_ptr<FakeHal>& fake, Epoch epoch) {
    std::lock_guard<std::mutex> guard(gLock);
    gHal = fake; gHalVersion = 3; gConnectionEpoch = epoch; gConnectionReady = true;
}
static void post(const sp<IGraphicBufferProducer>& producer) {
    sp<::android::Surface> surface = new ::android::Surface(producer);
    ANativeWindow_Buffer buffer{};
    assert(ANativeWindow_lock(surface.get(), &buffer, nullptr) == 0);
    std::memset(buffer.bits, 0x55, buffer.stride * buffer.height * 4);
    assert(ANativeWindow_unlockAndPost(surface.get()) == 0);
}
int main() {
    int cases = 0;
    auto fake = ndk::SharedRefBase::make<FakeHal>();
    connect(fake, 5);
    { std::lock_guard<std::mutex> guard(gLock); gHalVersion = 4; gConfigurations = {}; }
    auto callbacks = ndk::SharedRefBase::make<CallbackImpl>(5);
    callbacks->onStreamsConfiguredForConfiguration(0, 10, 16, 16, 30);
    sp<IGraphicBufferProducer> tokenQueue;
    { CallbackScope scope(5, 10); tokenQueue = createRelayQueue(0, 16, 16); }
    assert(tokenQueue && gSlots[0]->configurationToken == 10);
    post(tokenQueue); assert(fake->tokenSeen == 10);
    callbacks->onStreamsConfiguredForConfiguration(0, 20, 16, 16, 30);
    auto before = gSlots[0];
    callbacks->onCameraClosedForConfiguration(0, 10);
    assert(gConfigurations[0].token == 20 && !gConfigurations[0].closed);
    { CallbackScope scope(5, 10); assert(!createRelayQueue(0, 16, 16)); }
    { CallbackScope scope(5, 20); assert(createRelayQueue(0, 16, 16)); }
    assert(gSlots[0] != before && gSlots[0]->configurationToken == 20);
    nativeReleaseSurfaceForEpoch(nullptr, nullptr, 0, 5, 10);
    assert(gSlots[0] && gSlots[0]->configurationToken == 20);
    ++cases;
    callbacks->onCameraClosedForConfiguration(0, 30);
    callbacks->onStreamsConfiguredForConfiguration(0, 30, 16, 16, 30);
    assert(gConfigurations[0].token == 30 && gConfigurations[0].closed);
    { CallbackScope scope(5, 30); assert(!createRelayQueue(0, 16, 16)); }
    callbacks->onStreamsConfiguredForConfiguration(0, 40, 16, 16, 30);
    assert(gConfigurations[0].token == 40 && !gConfigurations[0].closed);
    releaseQueue(0, 5);
    fake->calls = 0;
    ++cases;
    connect(fake, 10);
    auto first = createRelayQueue(0, 16, 16); assert(first);
    auto old = gSlots[0];
    post(first); assert(fake->calls == 1 && old->held);
    auto second = createRelayQueue(0, 16, 16); assert(second);
    auto current = gSlots[0];
    current->consumer->setFrameAvailableListener(nullptr);
    post(second); assert(fake->calls == 1);
    old->listener->onFrameAvailable(BufferItem{}); // MUST NOT drain the new queue
    assert(fake->calls == 1 && !current->held);
    current->listener->onFrameAvailable(BufferItem{});
    assert(fake->calls == 2 && current->held && !old->held);
    current->consumer->setFrameAvailableListener(current->listener);
    ++cases;

    fake->pause();
    std::thread pump([&] { post(second); });
    fake->waitEntered();
    std::atomic<bool> replacing{false}, replaced{false};
    sp<IGraphicBufferProducer> third;
    std::thread replacement([&] {
        replacing = true; third = createRelayQueue(0, 32, 32); replaced = true;
    });
    while (!replacing) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(!replaced); // frame RPC and held-buffer commit precede queue replacement
    fake->resume(); pump.join(); replacement.join();
    assert(third && gSlots[0] != current && !current->held);
    ++cases;

    connect(fake, 20);
    {
        CallbackScope stale(10);
        assert(!createRelayQueue(0, 16, 16));
        int before = fake->presence;
        nativeSetCameraPresent(nullptr, nullptr, 0, JNI_FALSE);
        assert(fake->presence == before);
    }
    assert(gCallbackEpoch == 0);
    auto fresh = createRelayQueue(0, 16, 16); assert(fresh);
    auto freshQueue = gSlots[0];
    {
        CallbackScope outer(20);
        { CallbackScope inner(10); assert(nativeCallbackEpoch(nullptr, nullptr) == 10); }
        assert(nativeCallbackEpoch(nullptr, nullptr) == 20);
    }
    ++cases;
    nativeReleaseSurfaceForEpoch(nullptr, nullptr, 0, 10); // delayed old Java cleanup
    assert(gSlots[0] == freshQueue);
    onHalDiedCb(reinterpret_cast<void*>(Epoch(10))); // old connection death cannot clear replacement
    assert(isCurrentConnection(20) && gSlots[0] == freshQueue);
    ++cases;
    onHalDiedCb(reinterpret_cast<void*>(Epoch(20)));
    assert(!gHal && !gSlots[0]);
    ++cases;

    connect(fake, 30);
    auto a = createRelayQueue(0, 16, 16), b = createRelayQueue(1, 16, 16);
    fake->pause(); std::thread blocked([&] { post(a); }); fake->waitEntered();
    std::atomic<bool> otherDone{false};
    std::thread independent([&] { post(b); otherDone = true; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!otherDone && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(otherDone); // no global lock held over the other slot's HAL RPC
    independent.join(); fake->resume(); blocked.join();
    releaseQueue(0, 30); releaseQueue(1, 30);
    ++cases;

    std::thread scopeA([] { CallbackScope scope(101); assert(gCallbackEpoch == 101); });
    std::thread scopeB([] { CallbackScope scope(202); assert(gCallbackEpoch == 202); });
    scopeA.join(); scopeB.join(); assert(gCallbackEpoch == 0);
    ++cases;

    connect(fake, 50);
    auto old0 = createRelayQueue(0, 16, 16);
    assert(createRelayQueue(1, 16, 16));
    fake->pause(); std::thread inFlight([&] { post(old0); }); fake->waitEntered();
    std::thread death([] { onHalDiedCb(reinterpret_cast<void*>(Epoch(50))); });
    bool cleared = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!cleared && std::chrono::steady_clock::now() < end) {
        { std::lock_guard<std::mutex> guard(gLock); cleared = gConnectionEpoch == 0; }
        std::this_thread::yield();
    }
    assert(cleared); // old death is now paused behind slot-0's RPC
    connect(fake, 60);
    assert(createRelayQueue(1, 32, 32));
    auto replacement1 = gSlots[1];
    fake->resume(); inFlight.join(); death.join();
    assert(isCurrentConnection(60) && gSlots[1] == replacement1);
    releaseQueue(1, 60);
    ++cases;

    connect(fake, 70);
    auto rejected = createRelayQueue(0, 16, 16); post(rejected);
    auto rejectedQueue = gSlots[0]; assert(rejectedQueue && rejectedQueue->held);
    auto heldNumber = rejectedQueue->held->mFrameNumber;
    fake->failure = 1; post(rejected);
    assert(gSlots[0] == rejectedQueue && rejectedQueue->held->mFrameNumber == heldNumber);
    ++cases;
    fake->failure = 2; post(rejected);
    assert(!gSlots[0] && !rejectedQueue->held); // ambiguous acceptance -> abandon, never recycle
    fake->failure = 0;
    ++cases;
    // A delayed callback from a removed producer must not reach its replacement,
    // even when its token is greater than the last observed token.
    connect(fake, 101);
    {std::lock_guard<std::mutex> lock(gLock);gHalVersion=6;gRegistrations[0]=43;gConfigurations[0]={};}
    auto scoped=ndk::SharedRefBase::make<CallbackImpl>(101);
    scoped->onStreamsConfiguredForProducer(0,43,1000,16,16,30);
    assert(gConfigurations[0].token==1000&&!gConfigurations[0].closed);
    {std::lock_guard<std::mutex> lock(gLock);gRegistrations[0]=44;gConfigurations[0].closed=true;}
    scoped->onStreamsConfiguredForProducer(0,43,2000,16,16,60);
    assert(gConfigurations[0].token==1000&&gConfigurations[0].closed);
    scoped->onStreamsConfiguredForProducer(0,44,2001,16,16,60);
    scoped->onCameraClosedForProducer(0,43,3000);
    scoped->onStreamsConfiguredForConfiguration(0,4000,16,16,30);
    assert(gConfigurations[0].token==2001&&!gConfigurations[0].closed);
    ++cases;
    std::printf("PASS: %d production-relay queue/epoch/forced-interleaving cases\n", cases);
}
