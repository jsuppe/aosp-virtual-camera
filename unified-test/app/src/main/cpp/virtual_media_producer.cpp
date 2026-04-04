/*
 * Virtual Media Producer JNI - Bridge between Kotlin and native producers
 */

#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include "audio_producer.h"
#include "video_producer.h"

#define LOG_TAG "VirtualMediaProducer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static AudioProducer* gAudioProducer = nullptr;
static VideoProducer* gVideoProducer = nullptr;

extern "C" {

// ============================================================================
// Audio Producer - Service Mode (fd-based)
// ============================================================================

JNIEXPORT jboolean JNICALL
Java_com_example_vcamtest_NativeProducer_startAudioWithFd(
        JNIEnv* env, jobject /* this */,
        jint fd, jint bufferSize, jint headerSize,
        jint sampleRate, jint channels,
        jfloat frequency, jfloat amplitude) {
    
    if (gAudioProducer == nullptr) {
        gAudioProducer = new AudioProducer();
    }
    
    bool result = gAudioProducer->startWithFd(fd, bufferSize, headerSize,
                                               sampleRate, channels,
                                               frequency, amplitude);
    LOGI("startAudioWithFd(fd=%d, size=%d, header=%d, rate=%d, ch=%d, freq=%.1f, amp=%.2f) = %s",
         fd, bufferSize, headerSize, sampleRate, channels, frequency, amplitude,
         result ? "true" : "false");
    return result ? JNI_TRUE : JNI_FALSE;
}

// ============================================================================
// Audio Producer - Legacy Mode (direct socket)
// ============================================================================

JNIEXPORT jboolean JNICALL
Java_com_example_vcamtest_NativeProducer_startAudio(
        JNIEnv* env, jobject /* this */, jfloat frequency, jfloat amplitude) {
    
    if (gAudioProducer == nullptr) {
        gAudioProducer = new AudioProducer();
    }
    
    bool result = gAudioProducer->start(frequency, amplitude);
    LOGI("startAudio(%.1f Hz, %.2f) = %s", frequency, amplitude, result ? "true" : "false");
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_example_vcamtest_NativeProducer_stopAudio(JNIEnv* env, jobject /* this */) {
    if (gAudioProducer != nullptr) {
        gAudioProducer->stop();
        LOGI("stopAudio()");
    }
}

JNIEXPORT jboolean JNICALL
Java_com_example_vcamtest_NativeProducer_isAudioRunning(JNIEnv* env, jobject /* this */) {
    return (gAudioProducer != nullptr && gAudioProducer->isRunning()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_example_vcamtest_NativeProducer_setAudioFrequency(
        JNIEnv* env, jobject /* this */, jfloat frequency) {
    if (gAudioProducer != nullptr) {
        gAudioProducer->setFrequency(frequency);
    }
}

JNIEXPORT void JNICALL
Java_com_example_vcamtest_NativeProducer_setAudioAmplitude(
        JNIEnv* env, jobject /* this */, jfloat amplitude) {
    if (gAudioProducer != nullptr) {
        gAudioProducer->setAmplitude(amplitude);
    }
}

// ============================================================================
// Video Producer - Service Mode (fd-based)
// ============================================================================

JNIEXPORT jboolean JNICALL
Java_com_example_vcamtest_NativeProducer_startVideoWithFd(
        JNIEnv* env, jobject /* this */,
        jint fd, jint bufferSize, jint headerSize,
        jint width, jint height, jint fps) {
    
    if (gVideoProducer == nullptr) {
        gVideoProducer = new VideoProducer();
    }
    
    bool result = gVideoProducer->startWithFd(fd, bufferSize, headerSize, width, height, fps);
    LOGI("startVideoWithFd(fd=%d, size=%d, header=%d, %dx%d @ %dfps) = %s",
         fd, bufferSize, headerSize, width, height, fps,
         result ? "true" : "false");
    return result ? JNI_TRUE : JNI_FALSE;
}

// ============================================================================
// Video Producer - Legacy Mode (direct socket)
// ============================================================================

JNIEXPORT jboolean JNICALL
Java_com_example_vcamtest_NativeProducer_startVideo(
        JNIEnv* env, jobject /* this */, jint width, jint height, jint fps) {
    
    if (gVideoProducer == nullptr) {
        gVideoProducer = new VideoProducer();
    }
    
    bool result = gVideoProducer->start(width, height, fps);
    LOGI("startVideo(%dx%d @ %d fps) = %s", width, height, fps, result ? "true" : "false");
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_example_vcamtest_NativeProducer_stopVideo(JNIEnv* env, jobject /* this */) {
    if (gVideoProducer != nullptr) {
        gVideoProducer->stop();
        LOGI("stopVideo()");
    }
}

JNIEXPORT jboolean JNICALL
Java_com_example_vcamtest_NativeProducer_isVideoRunning(JNIEnv* env, jobject /* this */) {
    return (gVideoProducer != nullptr && gVideoProducer->isRunning()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_example_vcamtest_NativeProducer_setVideoPattern(
        JNIEnv* env, jobject /* this */, jint pattern) {
    if (gVideoProducer != nullptr) {
        gVideoProducer->setPattern(static_cast<VideoProducer::Pattern>(pattern));
    }
}

JNIEXPORT void JNICALL
Java_com_example_vcamtest_NativeProducer_setPreviewSurface(
        JNIEnv* env, jobject /* this */, jobject surface) {
    if (gVideoProducer == nullptr) {
        gVideoProducer = new VideoProducer();
    }
    
    ANativeWindow* window = nullptr;
    if (surface != nullptr) {
        window = ANativeWindow_fromSurface(env, surface);
        if (window) {
            // Set the surface format to RGBA
            ANativeWindow_setBuffersGeometry(window, 0, 0, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        }
    }
    
    gVideoProducer->setPreviewSurface(window);
    
    if (window) {
        ANativeWindow_release(window);  // setPreviewSurface acquires it
    }
    
    LOGI("setPreviewSurface(%p)", surface);
}

}  // extern "C"
