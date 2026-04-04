/**
 * Simple Renderer - Minimal CPU rendering for IPC throughput testing
 * 
 * Just fills buffer with animated color bars - no Vulkan overhead.
 */

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <chrono>
#include <cstring>
#include <vector>

#include "VirtualCameraClient.h"

#define LOG_TAG "SimpleRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct SimpleRenderer {
    ANativeWindow* window = nullptr;
    vcam::VirtualCameraClient client;
    std::vector<uint8_t> frameBuffer;
    int width = 0;
    int height = 0;
    uint64_t frameCount = 0;
    bool initialized = false;
    
    // FPS tracking
    int64_t lastFpsTime = 0;
    int fpsFrameCount = 0;
    int currentFps = 0;
    
    // Timing stats (microseconds)
    int64_t totalFillTime = 0;
    int64_t totalWriteTime = 0;
    int64_t totalLockTime = 0;
    int64_t totalMemcpyTime = 0;
    int64_t totalUnlockTime = 0;
    int timingSamples = 0;
};

static inline int64_t nowMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

// Ultra-fast animated pattern - just horizontal color bars that move
static void fillAnimatedBars(uint8_t* buffer, int width, int height, uint64_t frame) {
    const int barHeight = 64;
    int offset = (frame * 4) % barHeight;  // Scrolling animation
    
    // Pre-compute row colors
    uint32_t colors[] = {
        0xFF0000FF,  // Red
        0xFF00FF00,  // Green
        0xFFFF0000,  // Blue
        0xFFFFFF00,  // Cyan
        0xFFFF00FF,  // Magenta
        0xFF00FFFF,  // Yellow
        0xFFFFFFFF,  // White
        0xFF808080,  // Gray
    };
    int numColors = sizeof(colors) / sizeof(colors[0]);
    
    uint32_t* pixels = reinterpret_cast<uint32_t*>(buffer);
    
    for (int y = 0; y < height; y++) {
        int barIndex = ((y + offset) / barHeight) % numColors;
        uint32_t color = colors[barIndex];
        
        // Fill entire row with same color (very fast)
        uint32_t* row = pixels + y * width;
        for (int x = 0; x < width; x++) {
            row[x] = color;
        }
    }
}

// Even faster - use memset for solid frames, just change color each frame
static void fillSolidAnimated(uint8_t* buffer, int width, int height, uint64_t frame) {
    // Cycle through colors every 10 frames
    uint8_t r = (frame / 10) % 2 ? 255 : 0;
    uint8_t g = (frame / 20) % 2 ? 255 : 0;
    uint8_t b = (frame / 40) % 2 ? 255 : 0;
    
    uint32_t color = 0xFF000000 | (b << 16) | (g << 8) | r;
    
    uint32_t* pixels = reinterpret_cast<uint32_t*>(buffer);
    int numPixels = width * height;
    
    // Fill with 32-bit writes
    for (int i = 0; i < numPixels; i++) {
        pixels[i] = color;
    }
}

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_example_vcamtest_MainActivity_nativeCreateSimpleRenderer(
        JNIEnv* env, jobject /* this */, jobject surface, jint camWidth, jint camHeight) {
    
    auto* renderer = new SimpleRenderer();
    
    renderer->window = ANativeWindow_fromSurface(env, surface);
    if (!renderer->window) {
        LOGE("Failed to get native window");
        delete renderer;
        return 0;
    }
    
    renderer->width = camWidth;
    renderer->height = camHeight;
    LOGI("Simple renderer: %dx%d", camWidth, camHeight);
    
    // Initialize ashmem client
    if (!renderer->client.initialize(camWidth, camHeight)) {
        LOGE("Failed to initialize VirtualCameraClient");
        ANativeWindow_release(renderer->window);
        delete renderer;
        return 0;
    }
    
    // Allocate frame buffer
    size_t bufferSize = camWidth * camHeight * 4;
    renderer->frameBuffer.resize(bufferSize);
    LOGI("Frame buffer: %zu bytes (%.1f MB)", bufferSize, bufferSize / 1048576.0f);
    
    // Set window format
    ANativeWindow_setBuffersGeometry(renderer->window, 
        camWidth, camHeight, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    
    renderer->initialized = true;
    renderer->lastFpsTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    LOGI("Simple renderer created successfully");
    return reinterpret_cast<jlong>(renderer);
}

JNIEXPORT jint JNICALL
Java_com_example_vcamtest_MainActivity_nativeRenderSimpleFrame(
        JNIEnv* /* env */, jobject /* this */, jlong rendererPtr) {
    
    auto* renderer = reinterpret_cast<SimpleRenderer*>(rendererPtr);
    if (!renderer || !renderer->initialized) {
        return -1;
    }
    
    // === STAGE 1: Fill buffer ===
    int64_t t0 = nowMicros();
    fillAnimatedBars(renderer->frameBuffer.data(), 
                     renderer->width, renderer->height, 
                     renderer->frameCount);
    int64_t t1 = nowMicros();
    
    // === STAGE 2: Write to shared memory (IPC to HAL) ===
    renderer->client.writeFrame(renderer->frameBuffer.data(), 
                                renderer->frameBuffer.size());
    int64_t t2 = nowMicros();
    
    // === STAGE 3: Display window copy (broken down) ===
    int64_t tLock0 = nowMicros();
    ANativeWindow_Buffer buffer;
    int lockResult = ANativeWindow_lock(renderer->window, &buffer, nullptr);
    int64_t tLock1 = nowMicros();
    
    if (lockResult == 0) {
        uint8_t* dst = static_cast<uint8_t*>(buffer.bits);
        int dstStride = buffer.stride * 4;
        int srcStride = renderer->width * 4;
        
        for (int y = 0; y < renderer->height && y < buffer.height; y++) {
            memcpy(dst + y * dstStride, 
                   renderer->frameBuffer.data() + y * srcStride,
                   std::min(srcStride, dstStride));
        }
        int64_t tMemcpy1 = nowMicros();
        
        ANativeWindow_unlockAndPost(renderer->window);
        int64_t tUnlock1 = nowMicros();
        
        renderer->totalLockTime += (tLock1 - tLock0);
        renderer->totalMemcpyTime += (tMemcpy1 - tLock1);
        renderer->totalUnlockTime += (tUnlock1 - tMemcpy1);
    }
    
    // Accumulate timing stats
    renderer->totalFillTime += (t1 - t0);
    renderer->totalWriteTime += (t2 - t1);
    renderer->timingSamples++;
    
    renderer->frameCount++;
    
    // Calculate FPS
    renderer->fpsFrameCount++;
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    if (now - renderer->lastFpsTime >= 1000) {
        renderer->currentFps = renderer->fpsFrameCount;
        
        // Calculate average times per frame
        if (renderer->timingSamples > 0) {
            float avgFill = renderer->totalFillTime / (float)renderer->timingSamples / 1000.0f;
            float avgWrite = renderer->totalWriteTime / (float)renderer->timingSamples / 1000.0f;
            float avgLock = renderer->totalLockTime / (float)renderer->timingSamples / 1000.0f;
            float avgMemcpy = renderer->totalMemcpyTime / (float)renderer->timingSamples / 1000.0f;
            float avgUnlock = renderer->totalUnlockTime / (float)renderer->timingSamples / 1000.0f;
            float total = avgFill + avgWrite + avgLock + avgMemcpy + avgUnlock;
            
            LOGI("FPS: %d | Fill: %.1fms | SHM: %.1fms | Lock: %.1fms | Memcpy: %.1fms | Unlock: %.1fms | Total: %.1fms",
                 renderer->currentFps, avgFill, avgWrite, avgLock, avgMemcpy, avgUnlock, total);
            
            // Reset timing accumulators
            renderer->totalFillTime = 0;
            renderer->totalWriteTime = 0;
            renderer->totalLockTime = 0;
            renderer->totalMemcpyTime = 0;
            renderer->totalUnlockTime = 0;
            renderer->timingSamples = 0;
        }
        
        renderer->fpsFrameCount = 0;
        renderer->lastFpsTime = now;
    }
    
    return renderer->currentFps;
}

JNIEXPORT void JNICALL
Java_com_example_vcamtest_MainActivity_nativeDestroySimpleRenderer(
        JNIEnv* /* env */, jobject /* this */, jlong rendererPtr) {
    
    auto* renderer = reinterpret_cast<SimpleRenderer*>(rendererPtr);
    if (renderer) {
        renderer->client.shutdown();
        if (renderer->window) {
            ANativeWindow_release(renderer->window);
        }
        delete renderer;
        LOGI("Simple renderer destroyed");
    }
}

}  // extern "C"
