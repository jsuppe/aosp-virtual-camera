/**
 * Unified Renderer - Renders to both display and shared memory
 * 
 * For initial testing, this uses a simple CPU-based renderer.
 * Can be upgraded to full Vulkan later.
 */

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <cmath>
#include <cstring>
#include <vector>

#include "virtual_camera_writer.h"

#define LOG_TAG "UnifiedRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct UnifiedRenderer {
    ANativeWindow* window = nullptr;
    vcam::VirtualCameraWriter writer;
    std::vector<uint8_t> frameBuffer;
    int width = 0;
    int height = 0;
    int frameCount = 0;
    bool initialized = false;
};

// Render a rotating colored pattern (simple test pattern)
static void renderTestPattern(uint8_t* buffer, int width, int height, int frame) {
    float time = frame * 0.05f;
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            
            // Create a rotating gradient pattern
            float fx = (float)x / width - 0.5f;
            float fy = (float)y / height - 0.5f;
            
            // Rotate coordinates
            float angle = time;
            float rx = fx * cosf(angle) - fy * sinf(angle);
            float ry = fx * sinf(angle) + fy * cosf(angle);
            
            // Golden color with animated pattern
            float dist = sqrtf(rx * rx + ry * ry);
            float wave = sinf(dist * 10.0f - time * 2.0f) * 0.5f + 0.5f;
            
            // Golden cube-like colors
            uint8_t r = (uint8_t)(200 * wave + 55);
            uint8_t g = (uint8_t)(150 * wave + 50);
            uint8_t b = (uint8_t)(50 * wave + 20);
            
            // Add grid lines for visual reference
            if ((x % 64 < 2) || (y % 64 < 2)) {
                r = 255; g = 255; b = 255;
            }
            
            // Frame counter in corner (for latency measurement)
            if (x < 64 && y < 16) {
                int bit = (frame >> (x / 4)) & 1;
                r = bit ? 255 : 0;
                g = bit ? 255 : 0;
                b = bit ? 255 : 0;
            }
            
            buffer[idx + 0] = r;
            buffer[idx + 1] = g;
            buffer[idx + 2] = b;
            buffer[idx + 3] = 255;  // Alpha
        }
    }
}

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_example_vcamtest_MainActivity_nativeCreateRenderer(
        JNIEnv* env, jobject /* this */, jobject surface) {
    
    auto* renderer = new UnifiedRenderer();
    
    // Get native window from Surface
    renderer->window = ANativeWindow_fromSurface(env, surface);
    if (!renderer->window) {
        LOGE("Failed to get native window");
        delete renderer;
        return 0;
    }
    
    // Get window size
    renderer->width = ANativeWindow_getWidth(renderer->window);
    renderer->height = ANativeWindow_getHeight(renderer->window);
    LOGI("Window size: %dx%d", renderer->width, renderer->height);
    
    // Use fixed size for shared memory (camera resolution)
    int camWidth = 640;
    int camHeight = 480;
    
    // Initialize shared memory writer
    if (!renderer->writer.initialize(camWidth, camHeight)) {
        LOGE("Failed to initialize VirtualCameraWriter");
        ANativeWindow_release(renderer->window);
        delete renderer;
        return 0;
    }
    
    // Allocate frame buffer
    renderer->frameBuffer.resize(camWidth * camHeight * 4);
    
    // Set window format
    ANativeWindow_setBuffersGeometry(renderer->window, 
        camWidth, camHeight, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    
    renderer->initialized = true;
    LOGI("Renderer created successfully");
    
    return reinterpret_cast<jlong>(renderer);
}

JNIEXPORT void JNICALL
Java_com_example_vcamtest_MainActivity_nativeRenderFrame(
        JNIEnv* /* env */, jobject /* this */, jlong rendererPtr) {
    
    auto* renderer = reinterpret_cast<UnifiedRenderer*>(rendererPtr);
    if (!renderer || !renderer->initialized) {
        return;
    }
    
    int width = renderer->writer.getWidth();
    int height = renderer->writer.getHeight();
    
    // Render test pattern to buffer
    renderTestPattern(renderer->frameBuffer.data(), width, height, renderer->frameCount);
    
    // Write to shared memory (for HAL to read)
    renderer->writer.writeFrame(renderer->frameBuffer.data(), renderer->frameBuffer.size());
    
    // Also render to display window
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(renderer->window, &buffer, nullptr) == 0) {
        // Copy to window buffer
        uint8_t* dst = static_cast<uint8_t*>(buffer.bits);
        int dstStride = buffer.stride * 4;
        int srcStride = width * 4;
        
        for (int y = 0; y < height && y < buffer.height; y++) {
            memcpy(dst + y * dstStride, 
                   renderer->frameBuffer.data() + y * srcStride,
                   std::min(srcStride, dstStride));
        }
        
        ANativeWindow_unlockAndPost(renderer->window);
    }
    
    renderer->frameCount++;
    
    if (renderer->frameCount % 60 == 0) {
        LOGI("Rendered %d frames", renderer->frameCount);
    }
}

JNIEXPORT void JNICALL
Java_com_example_vcamtest_MainActivity_nativeDestroyRenderer(
        JNIEnv* /* env */, jobject /* this */, jlong rendererPtr) {
    
    auto* renderer = reinterpret_cast<UnifiedRenderer*>(rendererPtr);
    if (renderer) {
        renderer->writer.shutdown();
        if (renderer->window) {
            ANativeWindow_release(renderer->window);
        }
        delete renderer;
        LOGI("Renderer destroyed");
    }
}

}  // extern "C"
