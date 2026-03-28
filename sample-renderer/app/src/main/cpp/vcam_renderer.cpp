/**
 * vcam_renderer.cpp - JNI interface for Virtual Camera Renderer
 */
#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include "vulkan_renderer.h"

#define LOG_TAG "VCamRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_example_vcamrenderer_MainActivity_nativeInit(JNIEnv* env, jobject thiz) {
    LOGI("Initializing Vulkan renderer");
    
    auto* renderer = new VulkanRenderer();
    if (!renderer->initialize()) {
        LOGE("Failed to initialize Vulkan");
        delete renderer;
        return 0;
    }
    
    LOGI("Vulkan initialized successfully");
    return reinterpret_cast<jlong>(renderer);
}

JNIEXPORT void JNICALL
Java_com_example_vcamrenderer_MainActivity_nativeDestroy(JNIEnv* env, jobject thiz, jlong ctx) {
    auto* renderer = reinterpret_cast<VulkanRenderer*>(ctx);
    if (renderer) {
        renderer->shutdown();
        delete renderer;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_example_vcamrenderer_MainActivity_nativeSetSurface(
        JNIEnv* env, jobject thiz, jlong ctx, 
        jobject surface, jint width, jint height) {
    
    auto* renderer = reinterpret_cast<VulkanRenderer*>(ctx);
    if (!renderer) return JNI_FALSE;
    
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        LOGE("Failed to get ANativeWindow from Surface");
        return JNI_FALSE;
    }
    
    bool result = renderer->setSurface(window, width, height);
    // Don't release window - renderer owns it now
    
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_example_vcamrenderer_MainActivity_nativeStartRendering(JNIEnv* env, jobject thiz, jlong ctx) {
    auto* renderer = reinterpret_cast<VulkanRenderer*>(ctx);
    if (renderer) {
        renderer->startRenderLoop();
    }
}

JNIEXPORT void JNICALL
Java_com_example_vcamrenderer_MainActivity_nativeStopRendering(JNIEnv* env, jobject thiz, jlong ctx) {
    auto* renderer = reinterpret_cast<VulkanRenderer*>(ctx);
    if (renderer) {
        renderer->stopRenderLoop();
    }
}

JNIEXPORT void JNICALL
Java_com_example_vcamrenderer_MainActivity_nativeSetRotation(
        JNIEnv* env, jobject thiz, jlong ctx,
        jfloat angleX, jfloat angleY) {
    
    auto* renderer = reinterpret_cast<VulkanRenderer*>(ctx);
    if (renderer) {
        renderer->setRotation(angleX, angleY);
    }
}

} // extern "C"
