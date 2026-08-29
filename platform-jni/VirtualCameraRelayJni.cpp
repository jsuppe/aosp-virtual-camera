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
#include <ui/GraphicBuffer.h>
#include <utils/Log.h>

#include <mutex>

namespace {

namespace vhal = ::aidl::android::hardware::virtualcamera::hal;
using ::android::BufferItem;
using ::android::BufferItemConsumer;
using ::android::BufferQueue;
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
std::shared_ptr<vhal::IVirtualCameraHalCallback> gCallback;
::android::sp<BufferItemConsumer> gConsumer;
uint64_t gFramesPushed = 0;

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
    gHal = h;
    ALOGI("Connected to %s", kHalName);
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

        BufferItem item;
        if (consumer->acquireBuffer(&item, 0) != ::android::OK) return;
        sp<GraphicBuffer> gb = item.mGraphicBuffer;
        if (gb != nullptr) {
            auto h = hal();
            if (h != nullptr) {
                // Zero-copy: dup the gralloc handle into the stable AIDL
                // NativeHandle; the HAL clones it into an AHardwareBuffer.
                auto aidlHandle = ::android::dupToAidl(gb->handle);
                auto status = h->queueFrame(
                        aidlHandle,
                        static_cast<int32_t>(gb->getWidth()),
                        static_cast<int32_t>(gb->getHeight()),
                        static_cast<int32_t>(gb->getStride()),
                        static_cast<int32_t>(gb->getPixelFormat()),
                        static_cast<int64_t>(gb->getUsage()),
                        item.mTimestamp);
                if (status.isOk() && (++gFramesPushed % 150 == 0)) {
                    ALOGI("Pushed %llu frames across the boundary",
                          (unsigned long long)gFramesPushed);
                }
            }
        }
        consumer->releaseBuffer(item);
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
            GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN,
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
