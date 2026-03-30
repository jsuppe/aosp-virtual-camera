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
#include <chrono>
#include <cmath>
#include <cstdio>
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
    
    // FPS tracking
    int64_t startTimeMs = 0;
    int64_t lastFpsTime = 0;
    int fpsFrameCount = 0;
    int currentFps = 0;
};

// Simple 5x7 bitmap font for digits 0-9 and some chars
static const uint8_t FONT_5X7[16][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, // 2
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space (10)
    {0x00,0x04,0x04,0x04,0x04,0x00,0x04}, // : -> . (11)
    {0x0E,0x11,0x06,0x08,0x10,0x00,0x10}, // ? (12) - unused
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // - (13)
    {0x0E,0x11,0x06,0x04,0x04,0x00,0x04}, // F (14) - for FPS
    {0x1E,0x11,0x1E,0x14,0x12,0x11,0x00}, // fps text placeholder (15)
};

// Draw a character at position
static void drawChar(uint8_t* buffer, int width, int height, int cx, int cy, int charIdx, 
                     uint8_t r, uint8_t g, uint8_t b) {
    if (charIdx < 0 || charIdx > 15) return;
    const uint8_t* glyph = FONT_5X7[charIdx];
    
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (glyph[row] & (0x10 >> col)) {
                int px = cx + col;
                int py = cy + row;
                if (px >= 0 && px < width && py >= 0 && py < height) {
                    int idx = (py * width + px) * 4;
                    buffer[idx + 0] = r;
                    buffer[idx + 1] = g;
                    buffer[idx + 2] = b;
                    buffer[idx + 3] = 255;
                }
            }
        }
    }
}

// Draw a number at position (returns new x position)
static int drawNumber(uint8_t* buffer, int width, int height, int x, int y, int64_t num,
                      uint8_t r, uint8_t g, uint8_t b) {
    char str[32];
    snprintf(str, sizeof(str), "%lld", (long long)num);
    for (int i = 0; str[i]; i++) {
        int charIdx = (str[i] >= '0' && str[i] <= '9') ? (str[i] - '0') : 10;
        drawChar(buffer, width, height, x, y, charIdx, r, g, b);
        x += 6;
    }
    return x;
}

// Draw text overlay with frame info
static void drawOverlay(uint8_t* buffer, int width, int height, int frame, int64_t timestampMs, int fps) {
    // Background bar
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            buffer[idx + 0] = 0;
            buffer[idx + 1] = 0;
            buffer[idx + 2] = 0;
            buffer[idx + 3] = 200;
        }
    }
    
    int x = 4;
    int y = 6;
    
    // Frame: XXXXX
    drawChar(buffer, width, height, x, y, 14, 255, 255, 0);  // F
    x += 6;
    drawChar(buffer, width, height, x, y, 11, 255, 255, 0);  // :
    x += 6;
    x = drawNumber(buffer, width, height, x, y, frame, 255, 255, 255);
    
    x += 12;
    
    // Time: XXXXX ms
    drawChar(buffer, width, height, x, y, 7, 0, 255, 255);  // T (using 7)
    x += 6;
    drawChar(buffer, width, height, x, y, 11, 0, 255, 255);  // :
    x += 6;
    x = drawNumber(buffer, width, height, x, y, timestampMs, 255, 255, 255);
    
    x += 12;
    
    // FPS: XX
    x = drawNumber(buffer, width, height, x, y, fps, 0, 255, 0);
    drawChar(buffer, width, height, x, y, 14, 0, 255, 0);  // F
    x += 6;
    drawChar(buffer, width, height, x, y, 1, 0, 255, 0);   // P (using 1)
    x += 6;
    drawChar(buffer, width, height, x, y, 5, 0, 255, 0);   // S (using 5)
}

// Render a rotating colored pattern (simple test pattern)
static void renderTestPattern(uint8_t* buffer, int width, int height, int frame, 
                               int64_t timestampMs, int fps) {
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
            
            buffer[idx + 0] = r;
            buffer[idx + 1] = g;
            buffer[idx + 2] = b;
            buffer[idx + 3] = 255;
        }
    }
    
    // Draw overlay with frame info
    drawOverlay(buffer, width, height, frame, timestampMs, fps);
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
    
    // Get current time
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    // Initialize start time
    if (renderer->startTimeMs == 0) {
        renderer->startTimeMs = nowMs;
        renderer->lastFpsTime = nowMs;
    }
    
    // Calculate FPS every second
    renderer->fpsFrameCount++;
    if (nowMs - renderer->lastFpsTime >= 1000) {
        renderer->currentFps = renderer->fpsFrameCount;
        renderer->fpsFrameCount = 0;
        renderer->lastFpsTime = nowMs;
    }
    
    int64_t timestampMs = nowMs - renderer->startTimeMs;
    
    int width = renderer->writer.getWidth();
    int height = renderer->writer.getHeight();
    
    // Render test pattern to buffer with timestamp overlay
    renderTestPattern(renderer->frameBuffer.data(), width, height, 
                      renderer->frameCount, timestampMs, renderer->currentFps);
    
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
        LOGI("Rendered %d frames, %d FPS", renderer->frameCount, renderer->currentFps);
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
