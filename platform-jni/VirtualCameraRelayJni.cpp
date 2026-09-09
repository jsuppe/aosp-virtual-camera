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

#include <mutex>
#include <optional>

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
jmethodID gOnStreamsConfigured = nullptr;  // static (III)V
jmethodID gOnCameraClosed = nullptr;       // static ()V
jmethodID gOnHalDied = nullptr;            // static ()V

std::mutex gLock;
std::shared_ptr<vhal::IVirtualCameraHal> gHal;
int32_t gHalVersion = 0;                 // interface version the HAL implements
std::shared_ptr<vhal::IVirtualCameraHalCallback> gCallback;
::android::sp<BufferItemConsumer> gConsumer;
uint64_t gFramesPushed = 0;

// V2 protocol: the HAL keeps the newest buffer, so we keep it acquired here
// until the HAL retires it (on the next queueFrameFenced) and hands back the
// fence that says its GPU read is done. That fence goes to the BufferQueue
// as the release fence, so the producer's next render into the buffer waits.
std::optional<BufferItem> gHeld;

void releaseHeldLocked(const sp<BufferItemConsumer>& consumer,
                       ::ndk::ScopedFileDescriptor releaseFence) {   // fd -1 = none
    if (!gHeld || consumer == nullptr) { gHeld.reset(); return; }
    sp<Fence> fence = Fence::NO_FENCE;
    if (releaseFence.get() >= 0) {
        fence = new Fence(releaseFence.release());   // Fence owns the fd
    }
    consumer->releaseBuffer(*gHeld, fence);
    gHeld.reset();
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
        releaseHeldLocked(gConsumer, ::ndk::ScopedFileDescriptor());   // nobody is reading it any more
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
    ndk::ScopedAStatus onStreamsConfigured(int32_t w, int32_t h,
                                           int32_t fps) override {
        ALOGI("HAL -> onStreamsConfigured %dx%d@%d", w, h, fps);
        JNIEnv* env = attach();
        if (env && gNativeClass && gOnStreamsConfigured) {
            env->CallStaticVoidMethod(gNativeClass, gOnStreamsConfigured, w, h, fps);
        }
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus onCameraClosed() override {
        ALOGI("HAL -> onCameraClosed");
        JNIEnv* env = attach();
        if (env && gNativeClass && gOnCameraClosed) {
            env->CallStaticVoidMethod(gNativeClass, gOnCameraClosed);
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
    ALOGI("Connected to %s (HAL implements V%d; %s frame delivery)", kHalName, ver,
          ver >= 2 ? "fenced" : "unfenced V1");
    return gHal;
}

class FrameListener : public BufferItemConsumer::FrameAvailableListener {
public:
    void onFrameAvailable(const BufferItem&) override {
        sp<BufferItemConsumer> consumer;
        {
            std::lock_guard<std::mutex> lock(gLock);
            consumer = gConsumer;
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
                status = h->queueFrameFenced(
                        aidlHandle,
                        static_cast<int32_t>(gb->getWidth()),
                        static_cast<int32_t>(gb->getHeight()),
                        static_cast<int32_t>(gb->getStride()),
                        static_cast<int32_t>(gb->getPixelFormat()),
                        static_cast<int64_t>(gb->getUsage()),
                        item.mTimestamp, acquireFence, &retiredFence);
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
            if (queued && (++gFramesPushed % 150 == 0)) {
                ALOGI("Pushed %llu frames across the boundary%s",
                      (unsigned long long)gFramesPushed, fenced ? " (fenced)" : "");
            }
        }

        std::lock_guard<std::mutex> lock(gLock);
        if (fenced && queued) {
            // The HAL now holds `item` and has retired the previously held
            // buffer; return that one with the HAL's read-done fence.
            releaseHeldLocked(consumer, std::move(retiredFence));
            gHeld = item;
        } else {
            // V1 HAL (or the push failed): nothing downstream tracks this
            // buffer, so return it immediately, as before.
            consumer->releaseBuffer(item);
        }
    }
};

sp<FrameListener> gListener;

// ---- JNI methods ----

jboolean nativeIsHalUp(JNIEnv*, jclass) {
    return hal() != nullptr ? JNI_TRUE : JNI_FALSE;
}

void nativeSetProducerAvailable(JNIEnv*, jclass, jboolean available) {
    auto h = hal();
    if (h) h->setProducerAvailable(available == JNI_TRUE);
    else ALOGW("setProducerAvailable(%d): HAL not connected", available);
}

jobject nativeCreateSurface(JNIEnv* env, jclass, jint width, jint height) {
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);

    sp<BufferItemConsumer> bic = new BufferItemConsumer(
            consumer,
            // GPU producer (EGL) renders these; the HAL samples them as a GL
            // texture. HW_TEXTURE keeps them GPU-optimal (no CPU/linear layout).
            GRALLOC_USAGE_HW_TEXTURE,
            /*maxAcquiredBuffers*/ 2, /*controlledByApp*/ false);
    bic->setName(::android::String8("VCamPlatformRelay"));
    bic->setDefaultBufferSize(width, height);
    bic->setDefaultBufferFormat(::android::PIXEL_FORMAT_RGBA_8888);

    if (gListener == nullptr) gListener = new FrameListener();
    bic->setFrameAvailableListener(gListener);

    {
        std::lock_guard<std::mutex> lock(gLock);
        gConsumer = bic;
        gFramesPushed = 0;
    }
    ALOGI("Platform BufferQueue created (%dx%d), Surface for producer app", width, height);
    return android_view_Surface_createFromIGraphicBufferProducer(env, producer);
}

void nativeReleaseSurface(JNIEnv*, jclass) {
    sp<BufferItemConsumer> old;
    {
        std::lock_guard<std::mutex> lock(gLock);
        old = gConsumer;
        gConsumer = nullptr;
        releaseHeldLocked(old, ::ndk::ScopedFileDescriptor());   // queue is going away; HAL drops its frame on close
    }
    if (old != nullptr) {
        old->abandon();
        ALOGI("Platform BufferQueue released");
    }
}

const JNINativeMethod gMethods[] = {
    {"nativeIsHalUp", "()Z", (void*)nativeIsHalUp},
    {"nativeSetProducerAvailable", "(Z)V", (void*)nativeSetProducerAvailable},
    {"nativeCreateSurface", "(II)Landroid/view/Surface;", (void*)nativeCreateSurface},
    {"nativeReleaseSurface", "()V", (void*)nativeReleaseSurface},
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
            gNativeClass, "onStreamsConfiguredFromHal", "(III)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    gOnCameraClosed = env->GetStaticMethodID(
            gNativeClass, "onCameraClosedFromHal", "()V");
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
