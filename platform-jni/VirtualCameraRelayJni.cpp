/*
 * VirtualCameraRelayJni - platform-side frame pump for the stable-AIDL
 * virtual camera boundary.
 *
 * Loaded into system_server by VirtualCameraService. Owns the BufferQueue
 * (libgui is platform-only): the producer end is wrapped as a Java Surface
 * and relayed to the producer app; the consumer end forwards every frame's
 * gralloc handle zero-copy to the vendor HAL over the frozen
 * android.hardware.camera.virtual AIDL (ndk backend).
 *
 *   Java (VirtualCameraNative)  <->  this JNI lib  <->  IVirtualCameraHal (vendor APEX)
 */

#define LOG_TAG "VCamRelayJni"

#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include <aidl/android/hardware/virtualcamera/hal/IVirtualCameraHal.h>
#include <aidl/android/hardware/virtualcamera/hal/BnVirtualCameraHalCallback.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android/binder_manager.h>
#include <android/binder_ibinder.h>

// android_view_Surface.h relies on android::sp being declared, so it must
// come after the gui/utils headers.
#include <utils/StrongPointer.h>
#include <android_runtime/android_view_Surface.h>

#include <gui/BufferItem.h>
#include <gui/BufferItemConsumer.h>
#include <gui/BufferQueue.h>
#include <hardware/gralloc.h>
#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>
#include <utils/Log.h>

#include <array>
#include <vector>
#include <mutex>
#include <optional>
#include <memory>
#include <cstdint>

// Must match hal/core/VirtualCameraSlots.h (kMaxVirtualCameras).
constexpr int kMaxSlots = 4;

namespace {

namespace vhal = ::aidl::android::hardware::virtualcamera::hal;
using ::android::BufferItem;
using ::android::BufferItemConsumer;
using ::android::BufferQueue;
using ::android::Fence;
using ::android::GraphicBuffer;
using ::android::IGraphicBufferConsumer;
using ::android::IGraphicBufferProducer;
using ::android::sp;

JavaVM* gVm = nullptr;
jclass gNativeClass = nullptr;          // global ref: VirtualCameraNative
jmethodID gOnStreamsConfigured = nullptr;  // static (IIII)V  (slot, w, h, fps)
jmethodID gOnCameraClosed = nullptr;       // static (I)V     (slot)
jmethodID gOnHalDied = nullptr;            // static ()V

using Epoch = uintptr_t;
std::mutex gLock;
std::mutex gConnectLock;
std::shared_ptr<vhal::IVirtualCameraHal> gHal;
int32_t gHalVersion = 0;
std::shared_ptr<vhal::IVirtualCameraHalCallback> gCallback;
Epoch gConnectionEpoch = 0, gNextConnectionEpoch = 0;
bool gConnectionReady = false;

// Private JNI callback context. Async Java work snapshots this value and
// restores it through the scoped entry points below; no frozen AIDL change.
thread_local Epoch gCallbackEpoch = 0;
thread_local int64_t gCallbackConfiguration = 0;
thread_local int64_t gCallbackRegistration = 0;
std::array<int64_t,kMaxSlots> gRegistrations{};
struct ConfigurationState { int64_t token = 0; bool closed = false; };
std::array<ConfigurationState, kMaxSlots> gConfigurations{};
struct CallbackScope {
    Epoch previous;
    int64_t previousConfiguration, previousRegistration;
    explicit CallbackScope(Epoch epoch, int64_t token = 0, int64_t registration = 0)
        : previous(gCallbackEpoch), previousConfiguration(gCallbackConfiguration), previousRegistration(gCallbackRegistration) {
        gCallbackEpoch = epoch; gCallbackConfiguration = token; gCallbackRegistration = registration;
    }
    ~CallbackScope() { gCallbackEpoch = previous; gCallbackConfiguration = previousConfiguration; gCallbackRegistration = previousRegistration; }
};
bool isCurrentConnection(Epoch epoch) {
    std::lock_guard<std::mutex> lock(gLock);
    return epoch != 0 && gHal && gConnectionEpoch == epoch;
}

class FrameListener;
struct RelayQueue {
    sp<BufferItemConsumer> consumer;
    sp<FrameListener> listener;
    std::optional<BufferItem> held;
    Epoch connection = 0;
    uint64_t generation = 0, framesPushed = 0;
    int64_t configurationToken = 0;
};
// Serialize each frame RPC+commit with replacement/abandon for that slot.
// Never hold gLock over these operations or across a Java callback.
std::array<std::mutex, kMaxSlots> gSlotGates;
std::array<std::shared_ptr<RelayQueue>, kMaxSlots> gSlots;
std::array<uint64_t, kMaxSlots> gQueueGenerations{};

void releaseHeld(RelayQueue& queue, ndk::ScopedFileDescriptor releaseFence) {
    if (!queue.held) return;
    sp<Fence> fence = Fence::NO_FENCE;
    if (releaseFence.get() >= 0) fence = new Fence(releaseFence.release());
    queue.consumer->releaseBuffer(*queue.held, fence);
    queue.held.reset();
}
void abandonQueueLocked(int slot) {
    auto old = std::move(gSlots[slot]);
    if (!old) return;
    // Do NOT return a HAL-held buffer unfenced to a live queue. Abandon first:
    // the old producer can no longer recycle its buffers through this queue.
    old->consumer->abandon();
    old->held.reset();
}
void releaseQueue(int slot, Epoch expected, int64_t configurationToken = 0) {
    if (slot < 0 || slot >= kMaxSlots) return;
    std::lock_guard<std::mutex> gate(gSlotGates[slot]);
    if (!gSlots[slot] || (expected != 0 && gSlots[slot]->connection != expected)) return;
    if (configurationToken != 0 && gSlots[slot]->configurationToken != configurationToken) return;
    abandonQueueLocked(slot);
}

constexpr const char* kHalName =
        "android.hardware.virtualcamera.hal.IVirtualCameraHal/default";
JNIEnv* attach() {
    if (!gVm) return nullptr; // native test harness does not create a JVM
    JNIEnv* env = nullptr;
    if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    gVm->AttachCurrentThread(&env, nullptr);
    return env;
}

void onHalDiedCb(void* cookie) {
    const Epoch dead = reinterpret_cast<Epoch>(cookie);
    {
        std::lock_guard<std::mutex> lock(gLock);
        if (dead == 0 || dead != gConnectionEpoch || !gHal) return;
        gHal.reset(); gCallback.reset(); gHalVersion = 0;
        gConnectionEpoch = 0; gConnectionReady = false;
    }
    for (int slot = 0; slot < kMaxSlots; ++slot) releaseQueue(slot, dead);
    CallbackScope scope(dead);
    JNIEnv* env = attach();
    if (env && gNativeClass && gOnHalDied) env->CallStaticVoidMethod(gNativeClass, gOnHalDied);
}
AIBinder_DeathRecipient* deathRecipient() {
    static auto* recipient = AIBinder_DeathRecipient_new(&onHalDiedCb);
    return recipient;
}

class CallbackImpl : public vhal::BnVirtualCameraHalCallback {
public:
    explicit CallbackImpl(Epoch connection) : mConnection(connection) {}
    ndk::ScopedAStatus onStreamsConfigured(int32_t w, int32_t h, int32_t fps) override {
        return onStreamsConfiguredForCamera(0, w, h, fps);
    }
    ndk::ScopedAStatus onCameraClosed() override { return onCameraClosedForCamera(0); }
    ndk::ScopedAStatus onStreamsConfiguredForCamera(int32_t slot, int32_t w, int32_t h,
                                                   int32_t fps) override {
        return ndk::ScopedAStatus::fromServiceSpecificError(7); // no untokened V4 lifecycle
    }
    ndk::ScopedAStatus onCameraClosedForCamera(int32_t slot) override {
        return ndk::ScopedAStatus::fromServiceSpecificError(7);
    }
    ndk::ScopedAStatus onStreamsConfiguredForProducer(int32_t slot,int64_t registration,int64_t token,int32_t w,int32_t h,int32_t fps) override {
        if(!admit(slot,token,false,registration))return ndk::ScopedAStatus::ok();
        CallbackScope scope(mConnection,token,registration);
        JNIEnv* env=attach();if(env&&gNativeClass&&gOnStreamsConfigured)env->CallStaticVoidMethod(gNativeClass,gOnStreamsConfigured,slot,w,h,fps);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onCameraClosedForProducer(int32_t slot,int64_t registration,int64_t token) override {
        if(!admit(slot,token,true,registration))return ndk::ScopedAStatus::ok();
        CallbackScope scope(mConnection,token,registration);
        JNIEnv* env=attach();if(env&&gNativeClass&&gOnCameraClosed)env->CallStaticVoidMethod(gNativeClass,gOnCameraClosed,slot);
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onStreamsConfiguredForConfiguration(
            int32_t slot, int64_t token, int32_t w, int32_t h, int32_t fps) override {
        if (!admit(slot, token, false)) return ndk::ScopedAStatus::ok();
        CallbackScope scope(mConnection, token);
        JNIEnv* env = attach();
        if (env && gNativeClass && gOnStreamsConfigured) {
            env->CallStaticVoidMethod(gNativeClass, gOnStreamsConfigured, slot, w, h, fps);
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onCameraClosedForConfiguration(int32_t slot, int64_t token) override {
        if (!admit(slot, token, true)) return ndk::ScopedAStatus::ok();
        CallbackScope scope(mConnection, token);
        JNIEnv* env = attach();
        if (env && gNativeClass && gOnCameraClosed) {
            env->CallStaticVoidMethod(gNativeClass, gOnCameraClosed, slot);
        }
        return ndk::ScopedAStatus::ok();
    }
private:
    bool admit(int slot, int64_t token, bool closed, int64_t registration = 0) {
        std::lock_guard<std::mutex> lock(gLock);
        if (slot < 0 || slot >= kMaxSlots || token <= 0 || !gHal || gConnectionEpoch != mConnection) return false;
        if(gHalVersion>=6 && (registration<=0||gRegistrations[slot]!=registration))return false;
        auto& state = gConfigurations[slot];
        if (token < state.token || (token == state.token && (!closed || state.closed))) return false;
        state = {token, closed};
        return true;
    }
    const Epoch mConnection;
};

std::shared_ptr<vhal::IVirtualCameraHal> hal() {
    {
        std::lock_guard<std::mutex> lock(gLock);
        if (gHal && gConnectionReady) return gHal;
    }
    std::lock_guard<std::mutex> connect(gConnectLock);
    {
        std::lock_guard<std::mutex> lock(gLock);
        if (gHal && gConnectionReady) return gHal;
    }
    ndk::SpAIBinder binder(AServiceManager_checkService(kHalName));
    if (!binder.get()) return nullptr;
    auto h = vhal::IVirtualCameraHal::fromBinder(binder);
    if (!h) return nullptr;
    int32_t version = 0;
    if (!h->getInterfaceVersion(&version).isOk() || version < 6) return nullptr;
    Epoch epoch;
    std::shared_ptr<CallbackImpl> callback;
    {
        std::lock_guard<std::mutex> lock(gLock);
        epoch = ++gNextConnectionEpoch;
        if (epoch == 0) epoch = ++gNextConnectionEpoch; // zero is never a valid cookie
        callback = ndk::SharedRefBase::make<CallbackImpl>(epoch);
        gHal = h; gCallback = callback; gConnectionEpoch = epoch;
        gHalVersion = version; gConnectionReady = false;
        gConfigurations = {}; gRegistrations = {};
    }
    // No gLock across Binder RPC: callbacks/death are allowed to take it.
    void* cookie = reinterpret_cast<void*>(epoch); // opaque integer, never dereferenced/freed
    binder_status_t linked = AIBinder_linkToDeath(binder.get(), deathRecipient(), cookie);
    if (linked != STATUS_OK || !h->setCallback(callback).isOk()) {
        if (linked == STATUS_OK) AIBinder_unlinkToDeath(binder.get(), deathRecipient(), cookie);
        std::lock_guard<std::mutex> lock(gLock);
        if (gConnectionEpoch == epoch) {
            gHal.reset(); gCallback.reset(); gConnectionEpoch = 0;
            gHalVersion = 0; gConnectionReady = false;
        }
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(gLock);
    if (gConnectionEpoch != epoch || gHal != h) return nullptr;
    gConnectionReady = true;
    ALOGI("Connected to HAL V%d, epoch %llu", version, (unsigned long long)epoch);
    return h;
}

class FrameListener : public BufferItemConsumer::FrameAvailableListener {
public:
    FrameListener(int slot, const std::shared_ptr<RelayQueue>& queue) : mSlot(slot), mQueue(queue) {}
    void onFrameAvailable(const BufferItem&) override {
        std::lock_guard<std::mutex> gate(gSlotGates[mSlot]);
        auto queue = mQueue.lock();
        if (!queue || gSlots[mSlot] != queue) return; // old listener cannot drain a new queue
        std::shared_ptr<vhal::IVirtualCameraHal> h;
        int32_t version;
        {
            std::lock_guard<std::mutex> lock(gLock);
            if (!gHal || gConnectionEpoch != queue->connection) return;
            h = gHal; version = gHalVersion; // one consistent connection snapshot
        }
        const bool fenced = version >= 2;
        BufferItem item;
        if (queue->consumer->acquireBuffer(&item, 0, !fenced) != ::android::OK) return;
        auto gb = item.mGraphicBuffer;
        ndk::ScopedFileDescriptor retired;
        bool queued = false;
        if (gb) {
            auto handle = ::android::dupToAidl(gb->handle);
            ndk::ScopedAStatus status = ndk::ScopedAStatus::fromServiceSpecificError(3);
            ndk::ScopedFileDescriptor acquire;
            bool dependencyPreserved = true;
            if (fenced && item.mFence && item.mFence->isValid()) {
                acquire.set(item.mFence->dup());
                dependencyPreserved = acquire.get() >= 0;
            }
            if (dependencyPreserved) {
                if (version >= 4) {
                    status = h->queueFrameForConfiguration(mSlot, queue->configurationToken,
                            handle, gb->getWidth(), gb->getHeight(), gb->getStride(),
                            gb->getPixelFormat(), gb->getUsage(), item.mTimestamp, acquire, &retired);
                } else if (version >= 3) {
                    status = h->queueFrameForCamera(mSlot, handle, gb->getWidth(), gb->getHeight(),
                            gb->getStride(), gb->getPixelFormat(), gb->getUsage(),
                            item.mTimestamp, acquire, &retired);
                } else if (mSlot == 0 && fenced) {
                    status = h->queueFrameFenced(handle, gb->getWidth(), gb->getHeight(),
                            gb->getStride(), gb->getPixelFormat(), gb->getUsage(),
                            item.mTimestamp, acquire, &retired);
                } else if (mSlot == 0) {
                    status = h->queueFrame(handle, gb->getWidth(), gb->getHeight(),
                            gb->getStride(), gb->getPixelFormat(), gb->getUsage(), item.mTimestamp);
                }
            }
            if (dependencyPreserved && !status.isOk() &&
                    status.getExceptionCode() != EX_SERVICE_SPECIFIC) {
                // Transport failure does NOT prove that the remote HAL never
                // accepted/read the buffer. Disable recycling before dropping
                // any held/input references; never guess a completion fence.
                abandonQueueLocked(mSlot);
                return;
            }
            queued = dependencyPreserved && status.isOk();
        }
        if (fenced && queued) {
            releaseHeld(*queue, std::move(retired));
            queue->held = item;
        } else {
            // Failed submission never read the buffer; return its original
            // producer dependency, including when duplicating that fd failed.
            queue->consumer->releaseBuffer(item, item.mFence);
        }
        if (queued && ++queue->framesPushed % 150 == 0) {
            ALOGI("Pushed %llu frames (slot %d, queue %llu, connection %llu)",
                    (unsigned long long)queue->framesPushed, mSlot,
                    (unsigned long long)queue->generation, (unsigned long long)queue->connection);
        }
    }
private:
    const int mSlot;
    const std::weak_ptr<RelayQueue> mQueue;
};

sp<IGraphicBufferProducer> createRelayQueue(int slot, int width, int height) {
    if (slot < 0 || slot >= kMaxSlots || width <= 0 || height <= 0) return nullptr;
    std::lock_guard<std::mutex> gate(gSlotGates[slot]);
    Epoch epoch;
    {
        std::lock_guard<std::mutex> lock(gLock);
        epoch = gConnectionEpoch;
        if (!gHal || epoch == 0 || (gCallbackEpoch != 0 && gCallbackEpoch != epoch)) return nullptr;
        if (gHalVersion >= 4 && (gCallbackConfiguration <= 0 ||
                gConfigurations[slot].token != gCallbackConfiguration || gConfigurations[slot].closed)) return nullptr;
    }
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    auto queue = std::make_shared<RelayQueue>();
    queue->consumer = new BufferItemConsumer(consumer, GRALLOC_USAGE_HW_TEXTURE, 2, false);
    queue->consumer->setName(::android::String8(("VCamRelay" + std::to_string(slot)).c_str()));
    queue->consumer->setDefaultBufferSize(width, height);
    queue->consumer->setDefaultBufferFormat(::android::PIXEL_FORMAT_RGBA_8888);
    queue->connection = epoch;
    queue->configurationToken = gCallbackConfiguration;
    queue->generation = ++gQueueGenerations[slot];
    queue->listener = new FrameListener(slot, queue);
    queue->consumer->setFrameAvailableListener(queue->listener);
    if (!isCurrentConnection(epoch)) { queue->consumer->abandon(); return nullptr; }
    abandonQueueLocked(slot);
    bool installed = false;
    {
        std::lock_guard<std::mutex> lock(gLock);
        if (gHal && gConnectionEpoch == epoch && (gHalVersion < 4 ||
                (gConfigurations[slot].token == queue->configurationToken && !gConfigurations[slot].closed))) {
            gSlots[slot] = queue; // linearize installation against connection death
            installed = true;
        }
    }
    if (!installed) { queue->consumer->abandon(); return nullptr; }
    return producer;
}

// ---- JNI methods ----
jboolean nativeIsHalUp(JNIEnv*, jclass) { return hal() ? JNI_TRUE : JNI_FALSE; }
jint nativeGetMaxCameras(JNIEnv*, jclass) {
    auto h = hal();
    if (!h) return 0;
    int32_t version;
    {
        std::lock_guard<std::mutex> lock(gLock);
        if (gHal != h) return 0;
        version = gHalVersion;
    }
    if (version < 3) return 1;
    int32_t count = 1;
    if (!h->getMaxCameras(&count).isOk()) return 1;
    return count > kMaxSlots ? kMaxSlots : count;
}
jboolean nativeRegisterProducer(JNIEnv* env, jclass, jint slot, jlong registration, jintArray packed) {
    if (slot < 0 || slot >= kMaxSlots || registration <= 0 || !packed) return JNI_FALSE;
    const jsize count = env->GetArrayLength(packed);
    if (count < 4 || count > 400) return JNI_FALSE;
    std::vector<jint> v(count); env->GetIntArrayRegion(packed, 0, count, v.data());
    vhal::ProducerDescriptor d; d.registration=registration;
    d.facing=v[0]; d.orientation=v[1]; d.maxOutputStreams=v[2];
    if (v[3]<1 || v[3]>32) return JNI_FALSE;
    size_t at=4;
    for (int i=0;i<v[3];i++) {
        if (at+7>v.size()) return JNI_FALSE;
        vhal::ProducerMode m; m.id=v[at++];m.width=v[at++];m.height=v[at++];
        m.minFps=v[at++];m.maxFps=v[at++];m.allowDownscale=v[at++]!=0;
        int n=v[at++]; if(n<1||n>4||at+n>v.size())return JNI_FALSE;
        for(int j=0;j<n;j++)m.outputFormats.push_back(v[at++]);
        d.modes.push_back(m);
    }
    if(at!=v.size())return JNI_FALSE;
    auto h=hal(); if(!h)return JNI_FALSE;
    std::shared_ptr<vhal::IVirtualCameraHalCallback> owner;int64_t previous=0;
    { std::lock_guard<std::mutex> lock(gLock); if(gHal!=h)return JNI_FALSE; owner=gCallback; previous=gRegistrations[slot];gRegistrations[slot]=registration; }
    bool ok=h->registerProducer(slot,d,owner).isOk();
    if(!ok){std::lock_guard<std::mutex> lock(gLock);if(gHal==h&&gRegistrations[slot]==registration)gRegistrations[slot]=previous;}
    return ok;
}
void nativeRemoveProducer(JNIEnv*, jclass, jint slot, jlong registration) {
    auto h=hal(); if(!h)return;
    std::shared_ptr<vhal::IVirtualCameraHalCallback> owner;
    { std::lock_guard<std::mutex> lock(gLock); if(gHal!=h||slot<0||slot>=kMaxSlots||gRegistrations[slot]!=registration)return;
      owner=gCallback;gRegistrations[slot]=0;gConfigurations[slot].closed=true; }
    h->removeProducer(slot,registration,owner);
}
jboolean nativeSetConfigurationReady(JNIEnv*, jclass, jint slot, jlong connection, jlong token, jboolean ready) {
    auto h=hal(); if(!h)return JNI_FALSE;
    {std::lock_guard<std::mutex> lock(gLock);if(gHal!=h||gConnectionEpoch!=static_cast<Epoch>(connection))return JNI_FALSE;}
    return h->setConfigurationReady(slot,token,ready==JNI_TRUE).isOk();
}

// Legacy direct-call regression helper; no Java/JNI entry on descriptor-based cohorts.
[[maybe_unused]] void nativeSetCameraPresent(JNIEnv*, jclass, jint slot, jboolean present) {
    if (slot < 0 || slot >= kMaxSlots) return;
    auto h = hal();
    if (!h) return;
    int32_t version;
    {
        std::lock_guard<std::mutex> lock(gLock);
        if (gHal != h || (gCallbackEpoch != 0 && gCallbackEpoch != gConnectionEpoch)) return;
        version = gHalVersion;
    }
    if (version >= 3) h->setCameraPresent(slot, present == JNI_TRUE);
    else if (slot == 0) h->setProducerAvailable(present == JNI_TRUE);
}
jobject nativeCreateSurface(JNIEnv* env, jclass, jint slot, jint width, jint height) {
    auto producer = createRelayQueue(slot, width, height);
    return producer ? android_view_Surface_createFromIGraphicBufferProducer(env, producer) : nullptr;
}
void nativeReleaseSurface(JNIEnv*, jclass, jint slot) { releaseQueue(slot, gCallbackEpoch, gCallbackConfiguration); }
jlong nativeCallbackEpoch(JNIEnv*, jclass) { return static_cast<jlong>(gCallbackEpoch); }
jlong nativeCallbackConfiguration(JNIEnv*, jclass) { return gCallbackConfiguration; }
jboolean nativeIsCurrentConnection(JNIEnv*, jclass, jlong epoch) {
    return isCurrentConnection(static_cast<Epoch>(epoch)) ? JNI_TRUE : JNI_FALSE;
}
jobject nativeCreateSurfaceForEpoch(JNIEnv* env, jclass cls, jint slot, jint w, jint h, jlong epoch, jlong token) {
    // Zero is used only by the legacy unscoped Java facade. The async
    // callback facade rejects zero before entering JNI.
    CallbackScope scope(static_cast<Epoch>(epoch), token);
    return nativeCreateSurface(env, cls, slot, w, h);
}
void nativeReleaseSurfaceForEpoch(JNIEnv* env, jclass cls, jint slot, jlong epoch, jlong token = 0) {
    if (epoch == 0) return;
    CallbackScope scope(static_cast<Epoch>(epoch), token);
    nativeReleaseSurface(env, cls, slot);
}

jlong nativeCallbackRegistration(JNIEnv*,jclass){return gCallbackRegistration;}

const JNINativeMethod gMethods[] = {
    {"nativeIsHalUp", "()Z", (void*)nativeIsHalUp},
    {"nativeRegisterProducer", "(IJ[I)Z", (void*)nativeRegisterProducer},
    {"nativeRemoveProducer", "(IJ)V", (void*)nativeRemoveProducer},
    {"nativeSetConfigurationReady", "(IJJZ)Z", (void*)nativeSetConfigurationReady},
    {"nativeGetMaxCameras", "()I", (void*)nativeGetMaxCameras},
    {"nativeReleaseSurface", "(I)V", (void*)nativeReleaseSurface},
    {"nativeCallbackEpoch", "()J", (void*)nativeCallbackEpoch},
    {"nativeCallbackConfiguration", "()J", (void*)nativeCallbackConfiguration},
    {"nativeCallbackRegistration", "()J", (void*)nativeCallbackRegistration},
    {"nativeIsCurrentConnection", "(J)Z", (void*)nativeIsCurrentConnection},
    {"nativeCreateSurfaceForEpoch", "(IIIJJ)Landroid/view/Surface;", (void*)nativeCreateSurfaceForEpoch},
    {"nativeReleaseSurfaceForEpoch", "(IJJ)V", (void*)nativeReleaseSurfaceForEpoch},
};

}  // namespace

jint JNI_OnLoad(JavaVM* vm, void*) {
    gVm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    jclass clazz = env->FindClass(
            "com/android/server/camera/virtual/VirtualCameraNative");
    if (clazz == nullptr) {
        ALOGE("VirtualCameraNative class not found");
        return JNI_ERR;
    }
    gNativeClass = static_cast<jclass>(env->NewGlobalRef(clazz));
    gOnStreamsConfigured = env->GetStaticMethodID(
            gNativeClass, "onStreamsConfiguredFromHal", "(IIII)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    gOnCameraClosed = env->GetStaticMethodID(
            gNativeClass, "onCameraClosedFromHal", "(I)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    gOnHalDied = env->GetStaticMethodID(
            gNativeClass, "onHalDiedFromNative", "()V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!gOnStreamsConfigured || !gOnCameraClosed || !gOnHalDied) {
        ALOGE("Java callback methods missing (R8 stripped?) — callbacks disabled");
    }
    if (env->RegisterNatives(gNativeClass, gMethods,
                             sizeof(gMethods) / sizeof(gMethods[0])) != 0) {
        ALOGE("RegisterNatives failed");
        return JNI_ERR;
    }
    ALOGI("VCamRelayJni loaded");
    return JNI_VERSION_1_6;
}
