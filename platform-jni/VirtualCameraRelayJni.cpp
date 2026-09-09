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
#include <mutex>
#include <optional>

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

std::mutex gLock;
std::shared_ptr<vhal::IVirtualCameraHal> gHal;
int32_t gHalVersion = 0;                 // interface version the HAL implements
std::shared_ptr<vhal::IVirtualCameraHalCallback> gCallback;

// One relay queue per virtual camera slot (V3). Slot 0 is also what a V1/V2
// HAL sees through the unslotted methods.
struct Slot {
    ::android::sp<BufferItemConsumer> consumer;
    uint64_t framesPushed = 0;
    // V2+ protocol: the HAL keeps the newest buffer, so we keep it acquired
    // here until the HAL retires it (on the next queueFrame*) and hands back
    // the fence that says its GPU read is done. That fence goes to the
    // BufferQueue as the release fence, so the producer's next render into
    // the buffer waits.
    std::optional<BufferItem> held;
};
std::array<Slot, kMaxSlots> gSlots;

void releaseHeldLocked(Slot& s, ::ndk::ScopedFileDescriptor releaseFence) {   // fd -1 = none
    if (!s.held || s.consumer == nullptr) { s.held.reset(); return; }
    sp<Fence> fence = Fence::NO_FENCE;
    if (releaseFence.get() >= 0) {
        fence = new Fence(releaseFence.release());   // Fence owns the fd
    }
    s.consumer->releaseBuffer(*s.held, fence);
    s.held.reset();
}

constexpr const char* kHalName =
        "android.hardware.virtualcamera.hal.IVirtualCameraHal/default";

JNIEnv* attach() {
    JNIEnv* env = nullptr;
    if (gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }
    gVm->AttachCurrentThread(&env, nullptr);
    return env;
}

void onHalDiedCb(void* /*cookie*/) {
    ALOGW("IVirtualCameraHal died (APEX update / crash) — will reconnect");
    {
        std::lock_guard<std::mutex> lock(gLock);
        gHal = nullptr;
        gHalVersion = 0;
        for (auto& s : gSlots) releaseHeldLocked(s, ::ndk::ScopedFileDescriptor());   // nobody is reading them any more
    }
    JNIEnv* env = attach();
    if (env && gNativeClass && gOnHalDied) {
        env->CallStaticVoidMethod(gNativeClass, gOnHalDied);
    }
}

AIBinder_DeathRecipient* deathRecipient() {
    static AIBinder_DeathRecipient* dr =
            AIBinder_DeathRecipient_new(&onHalDiedCb);
    return dr;
}

class CallbackImpl : public vhal::BnVirtualCameraHalCallback {
public:
    // V1/V2 forms == slot 0.
    ndk::ScopedAStatus onStreamsConfigured(int32_t w, int32_t h, int32_t fps) override {
        return onStreamsConfiguredForCamera(0, w, h, fps);
    }
    ndk::ScopedAStatus onCameraClosed() override { return onCameraClosedForCamera(0); }
    // V3 forms.
    ndk::ScopedAStatus onStreamsConfiguredForCamera(int32_t slot, int32_t w, int32_t h,
                                                    int32_t fps) override {
        ALOGI("HAL -> onStreamsConfigured slot %d %dx%d@%d", slot, w, h, fps);
        JNIEnv* env = attach();
        if (env && gNativeClass && gOnStreamsConfigured) {
            env->CallStaticVoidMethod(gNativeClass, gOnStreamsConfigured, slot, w, h, fps);
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onCameraClosedForCamera(int32_t slot) override {
        ALOGI("HAL -> onCameraClosed slot %d", slot);
        JNIEnv* env = attach();
        if (env && gNativeClass && gOnCameraClosed) {
            env->CallStaticVoidMethod(gNativeClass, gOnCameraClosed, slot);
        }
        return ndk::ScopedAStatus::ok();
    }
};

/** Get (connecting if needed) the vendor HAL; registers callback + death. */
std::shared_ptr<vhal::IVirtualCameraHal> hal() {
    std::lock_guard<std::mutex> lock(gLock);
    if (gHal) return gHal;
    ndk::SpAIBinder binder(AServiceManager_checkService(kHalName));
    if (binder.get() == nullptr) {
        return nullptr;  // vendor HAL not up (yet)
    }
    auto h = vhal::IVirtualCameraHal::fromBinder(binder);
    if (!h) return nullptr;
    AIBinder_linkToDeath(binder.get(), deathRecipient(), nullptr);
    if (!gCallback) gCallback = ndk::SharedRefBase::make<CallbackImpl>();
    h->setCallback(gCallback);
    int32_t ver = 0;
    if (!h->getInterfaceVersion(&ver).isOk()) ver = 1;
    gHal = h;
    gHalVersion = ver;
    ALOGI("Connected to %s (HAL implements V%d; %s frame delivery; %s)", kHalName, ver,
          ver >= 2 ? "fenced" : "unfenced V1",
          ver >= 3 ? "multi-camera slots" : "single camera (slot 0)");
    return gHal;
}

class FrameListener : public BufferItemConsumer::FrameAvailableListener {
public:
    explicit FrameListener(int slot) : mSlot(slot) {}
    void onFrameAvailable(const BufferItem&) override {
        sp<BufferItemConsumer> consumer;
        {
            std::lock_guard<std::mutex> lock(gLock);
            consumer = gSlots[mSlot].consumer;
        }
        if (consumer == nullptr) return;

        auto h = hal();
        int32_t ver;
        {
            std::lock_guard<std::mutex> lock(gLock);
            ver = gHalVersion;
        }
        const bool fenced = (h != nullptr && ver >= 2);

        // V2: do not CPU-wait on the producer's fence here; it travels with
        // the frame and the HAL's GPU waits on it. V1 HALs know nothing of
        // fences, so keep waiting on their behalf (waitForFence=true).
        BufferItem item;
        if (consumer->acquireBuffer(&item, 0, /*waitForFence*/ !fenced) != ::android::OK) {
            return;
        }
        sp<GraphicBuffer> gb = item.mGraphicBuffer;
        bool queued = false;
        ::ndk::ScopedFileDescriptor retiredFence;   // for the buffer the HAL just gave up (-1 = none)
        if (gb != nullptr && h != nullptr) {
            // Zero-copy: dup the gralloc handle into the stable AIDL
            // NativeHandle; the HAL clones it into an AHardwareBuffer.
            auto aidlHandle = ::android::dupToAidl(gb->handle);
            ndk::ScopedAStatus status;
            if (fenced) {
                ::ndk::ScopedFileDescriptor acquireFence;   // -1 parcels as null
                if (item.mFence != nullptr && item.mFence->isValid()) {
                    acquireFence.set(item.mFence->dup());
                }
                if (ver >= 3) {
                    status = h->queueFrameForCamera(
                            mSlot, aidlHandle,
                            static_cast<int32_t>(gb->getWidth()),
                            static_cast<int32_t>(gb->getHeight()),
                            static_cast<int32_t>(gb->getStride()),
                            static_cast<int32_t>(gb->getPixelFormat()),
                            static_cast<int64_t>(gb->getUsage()),
                            item.mTimestamp, acquireFence, &retiredFence);
                } else if (mSlot == 0) {
                    status = h->queueFrameFenced(
                            aidlHandle,
                            static_cast<int32_t>(gb->getWidth()),
                            static_cast<int32_t>(gb->getHeight()),
                            static_cast<int32_t>(gb->getStride()),
                            static_cast<int32_t>(gb->getPixelFormat()),
                            static_cast<int64_t>(gb->getUsage()),
                            item.mTimestamp, acquireFence, &retiredFence);
                } else {
                    status = ndk::ScopedAStatus::fromServiceSpecificError(3);   // V2 HAL: slot 0 only
                }
            } else if (mSlot != 0) {
                status = ndk::ScopedAStatus::fromServiceSpecificError(3);
            } else {
                status = h->queueFrame(
                        aidlHandle,
                        static_cast<int32_t>(gb->getWidth()),
                        static_cast<int32_t>(gb->getHeight()),
                        static_cast<int32_t>(gb->getStride()),
                        static_cast<int32_t>(gb->getPixelFormat()),
                        static_cast<int64_t>(gb->getUsage()),
                        item.mTimestamp);
            }
            queued = status.isOk();
        }

        std::lock_guard<std::mutex> lock(gLock);
        Slot& s = gSlots[mSlot];
        if (queued && (++s.framesPushed % 150 == 0)) {
            ALOGI("Pushed %llu frames across the boundary (slot %d)%s",
                  (unsigned long long)s.framesPushed, mSlot, fenced ? " (fenced)" : "");
        }
        if (fenced && queued) {
            // The HAL now holds `item` and has retired the previously held
            // buffer; return that one with the HAL's read-done fence.
            releaseHeldLocked(s, std::move(retiredFence));
            s.held = item;
        } else {
            // V1 HAL (or the push failed): nothing downstream tracks this
            // buffer, so return it immediately, as before.
            consumer->releaseBuffer(item);
        }
    }
private:
    const int mSlot;
};

std::array<sp<FrameListener>, kMaxSlots> gListeners;

// ---- JNI methods ----

jboolean nativeIsHalUp(JNIEnv*, jclass) {
    return hal() != nullptr ? JNI_TRUE : JNI_FALSE;
}

jint nativeGetMaxCameras(JNIEnv*, jclass) {
    auto h = hal();
    if (!h) return 0;
    int32_t ver;
    {
        std::lock_guard<std::mutex> lock(gLock);
        ver = gHalVersion;
    }
    if (ver < 3) return 1;
    int32_t n = 1;
    if (!h->getMaxCameras(&n).isOk()) n = 1;
    return n > kMaxSlots ? kMaxSlots : n;
}

void nativeSetCameraPresent(JNIEnv*, jclass, jint slot, jboolean present) {
    auto h = hal();
    if (!h) { ALOGW("setCameraPresent(%d,%d): HAL not connected", slot, present); return; }
    if (slot < 0 || slot >= kMaxSlots) return;
    int32_t ver;
    {
        std::lock_guard<std::mutex> lock(gLock);
        ver = gHalVersion;
    }
    if (ver >= 3) h->setCameraPresent(slot, present == JNI_TRUE);
    else if (slot == 0) h->setProducerAvailable(present == JNI_TRUE);
    else ALOGW("slot %d ignored: HAL is V%d (single camera)", slot, ver);
}

jobject nativeCreateSurface(JNIEnv* env, jclass, jint slot, jint width, jint height) {
    if (slot < 0 || slot >= kMaxSlots) return nullptr;
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);

    sp<BufferItemConsumer> bic = new BufferItemConsumer(
            consumer,
            // GPU producer (EGL) renders these; the HAL samples them as a GL
            // texture. HW_TEXTURE keeps them GPU-optimal (no CPU/linear layout).
            GRALLOC_USAGE_HW_TEXTURE,
            /*maxAcquiredBuffers*/ 2, /*controlledByApp*/ false);
    bic->setName(::android::String8(("VCamPlatformRelay" + std::to_string(slot)).c_str()));
    bic->setDefaultBufferSize(width, height);
    bic->setDefaultBufferFormat(::android::PIXEL_FORMAT_RGBA_8888);

    if (gListeners[slot] == nullptr) gListeners[slot] = new FrameListener(slot);
    bic->setFrameAvailableListener(gListeners[slot]);

    sp<BufferItemConsumer> old;
    {
        std::lock_guard<std::mutex> lock(gLock);
        Slot& s = gSlots[slot];
        old = s.consumer;
        releaseHeldLocked(s, ::ndk::ScopedFileDescriptor());
        s.consumer = bic;
        s.framesPushed = 0;
    }
    if (old != nullptr) old->abandon();
    ALOGI("Platform BufferQueue created for slot %d (%dx%d), Surface for producer app",
          slot, width, height);
    return android_view_Surface_createFromIGraphicBufferProducer(env, producer);
}

void nativeReleaseSurface(JNIEnv*, jclass, jint slot) {
    if (slot < 0 || slot >= kMaxSlots) return;
    sp<BufferItemConsumer> old;
    {
        std::lock_guard<std::mutex> lock(gLock);
        Slot& s = gSlots[slot];
        old = s.consumer;
        s.consumer = nullptr;
        releaseHeldLocked(s, ::ndk::ScopedFileDescriptor());   // queue is going away; HAL drops its frame on close
    }
    if (old != nullptr) {
        old->abandon();
        ALOGI("Platform BufferQueue released (slot %d)", slot);
    }
}

const JNINativeMethod gMethods[] = {
    {"nativeIsHalUp", "()Z", (void*)nativeIsHalUp},
    {"nativeGetMaxCameras", "()I", (void*)nativeGetMaxCameras},
    {"nativeSetCameraPresent", "(IZ)V", (void*)nativeSetCameraPresent},
    {"nativeCreateSurface", "(III)Landroid/view/Surface;", (void*)nativeCreateSurface},
    {"nativeReleaseSurface", "(I)V", (void*)nativeReleaseSurface},
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
