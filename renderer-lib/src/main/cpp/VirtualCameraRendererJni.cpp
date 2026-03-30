/*
 * JNI wrapper for VirtualCameraRenderer
 */

#include <jni.h>
#include "VirtualCameraRenderer.h"

using namespace virtualcamera;

static VirtualCameraRenderer* sRenderer = nullptr;

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_example_virtualcamera_VirtualCameraRenderer_nativeInitialize(
        JNIEnv* env, jobject thiz, jint width, jint height, jint format) {
    
    if (sRenderer != nullptr) {
        sRenderer->shutdown();
        delete sRenderer;
    }
    
    sRenderer = new VirtualCameraRenderer();
    return sRenderer->initialize(width, height, static_cast<PixelFormat>(format));
}

JNIEXPORT void JNICALL
Java_com_example_virtualcamera_VirtualCameraRenderer_nativeShutdown(
        JNIEnv* env, jobject thiz) {
    
    if (sRenderer != nullptr) {
        sRenderer->shutdown();
        delete sRenderer;
        sRenderer = nullptr;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_example_virtualcamera_VirtualCameraRenderer_nativeIsReady(
        JNIEnv* env, jobject thiz) {
    
    return sRenderer != nullptr && sRenderer->isReady();
}

JNIEXPORT jobject JNICALL
Java_com_example_virtualcamera_VirtualCameraRenderer_nativeBeginFrame(
        JNIEnv* env, jobject thiz) {
    
    if (sRenderer == nullptr || !sRenderer->isReady()) {
        return nullptr;
    }
    
    void* buffer = sRenderer->beginFrame();
    if (buffer == nullptr) {
        return nullptr;
    }
    
    // Return a direct ByteBuffer wrapping the frame memory
    return env->NewDirectByteBuffer(buffer, sRenderer->getFrameSize());
}

JNIEXPORT void JNICALL
Java_com_example_virtualcamera_VirtualCameraRenderer_nativeEndFrame(
        JNIEnv* env, jobject thiz, jlong timestamp) {
    
    if (sRenderer != nullptr) {
        sRenderer->endFrame(static_cast<uint64_t>(timestamp));
    }
}

JNIEXPORT jint JNICALL
Java_com_example_virtualcamera_VirtualCameraRenderer_nativeGetWidth(
        JNIEnv* env, jobject thiz) {
    
    return sRenderer != nullptr ? sRenderer->getWidth() : 0;
}

JNIEXPORT jint JNICALL
Java_com_example_virtualcamera_VirtualCameraRenderer_nativeGetHeight(
        JNIEnv* env, jobject thiz) {
    
    return sRenderer != nullptr ? sRenderer->getHeight() : 0;
}

JNIEXPORT jint JNICALL
Java_com_example_virtualcamera_VirtualCameraRenderer_nativeGetFrameSize(
        JNIEnv* env, jobject thiz) {
    
    return sRenderer != nullptr ? static_cast<jint>(sRenderer->getFrameSize()) : 0;
}

}  // extern "C"
