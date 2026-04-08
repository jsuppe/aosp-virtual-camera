/**
 * Unified Renderer - Renders to both display and virtual camera
 *
 * Tries v2 (zero-copy AHardwareBuffer pool) first, falls back to
 * v1 (ashmem shared memory) if the HAL's v2 socket isn't available.
 */

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "virtual_camera_writer.h"
#include "virtual_camera_writer_v2.h"

#define LOG_TAG "UnifiedRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct UnifiedRenderer {
    ANativeWindow* window = nullptr;
    vcam::VirtualCameraWriter writerV1;
    vcam::VirtualCameraWriterV2 writerV2;
    bool usingV2 = false;

    std::vector<uint8_t> frameBuffer;  // Used for v1 path and display
    int width = 0;
    int height = 0;
    int camWidth = 0;
    int camHeight = 0;
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
    {0x0E,0x11,0x06,0x08,0x10,0x00,0x10}, // ? (12)
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // - (13)
    {0x0E,0x11,0x06,0x04,0x04,0x00,0x04}, // F (14)
    {0x1E,0x11,0x1E,0x14,0x12,0x11,0x00}, // fps text placeholder (15)
};

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

static void drawOverlay(uint8_t* buffer, int width, int height, int frame,
                        int64_t timestampMs, int fps, bool v2Active) {
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

    // V2 indicator
    if (v2Active) {
        // "V2" in green
        drawChar(buffer, width, height, x, y, 2, 0, 255, 0);
        x += 8;
    }

    // Frame: XXXXX
    drawChar(buffer, width, height, x, y, 14, 255, 255, 0);  // F
    x += 6;
    drawChar(buffer, width, height, x, y, 11, 255, 255, 0);  // :
    x += 6;
    x = drawNumber(buffer, width, height, x, y, frame, 255, 255, 255);

    x += 12;

    // Time: XXXXX ms
    drawChar(buffer, width, height, x, y, 7, 0, 255, 255);  // T
    x += 6;
    drawChar(buffer, width, height, x, y, 11, 0, 255, 255);  // :
    x += 6;
    x = drawNumber(buffer, width, height, x, y, timestampMs, 255, 255, 255);

    x += 12;

    // FPS: XX
    x = drawNumber(buffer, width, height, x, y, fps, 0, 255, 0);
    drawChar(buffer, width, height, x, y, 14, 0, 255, 0);  // F
    x += 6;
    drawChar(buffer, width, height, x, y, 1, 0, 255, 0);   // P
    x += 6;
    drawChar(buffer, width, height, x, y, 5, 0, 255, 0);   // S
}

static void renderTestPattern(uint8_t* buffer, int width, int height, int frame,
                               int64_t timestampMs, int fps, bool v2Active) {
    float time = frame * 0.05f;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;

            float fx = (float)x / width - 0.5f;
            float fy = (float)y / height - 0.5f;

            float angle = time;
            float rx = fx * cosf(angle) - fy * sinf(angle);
            float ry = fx * sinf(angle) + fy * cosf(angle);

            float dist = sqrtf(rx * rx + ry * ry);
            float wave = sinf(dist * 10.0f - time * 2.0f) * 0.5f + 0.5f;

            uint8_t r = (uint8_t)(200 * wave + 55);
            uint8_t g = (uint8_t)(150 * wave + 50);
            uint8_t b = (uint8_t)(50 * wave + 20);

            if ((x % 64 < 2) || (y % 64 < 2)) {
                r = 255; g = 255; b = 255;
            }

            buffer[idx + 0] = r;
            buffer[idx + 1] = g;
            buffer[idx + 2] = b;
            buffer[idx + 3] = 255;
        }
    }

    drawOverlay(buffer, width, height, frame, timestampMs, fps, v2Active);
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

    renderer->width = ANativeWindow_getWidth(renderer->window);
    renderer->height = ANativeWindow_getHeight(renderer->window);
    LOGI("Window size: %dx%d", renderer->width, renderer->height);

    // Camera resolution
    renderer->camWidth = 640;
    renderer->camHeight = 480;

    // Try v2 first (zero-copy AHardwareBuffer pool)
    if (renderer->writerV2.initialize(renderer->camWidth, renderer->camHeight)) {
        renderer->usingV2 = true;
        LOGI("Using V2 zero-copy writer");
    } else {
        LOGI("V2 not available, falling back to V1 shared memory");
        if (!renderer->writerV1.initialize(renderer->camWidth, renderer->camHeight)) {
            LOGE("Failed to initialize V1 writer");
            ANativeWindow_release(renderer->window);
            delete renderer;
            return 0;
        }
        renderer->usingV2 = false;
    }

    // Frame buffer for v1 path and display rendering
    renderer->frameBuffer.resize(renderer->camWidth * renderer->camHeight * 4);

    ANativeWindow_setBuffersGeometry(renderer->window,
        renderer->camWidth, renderer->camHeight,
        AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    renderer->initialized = true;
    LOGI("Renderer created (v%d)", renderer->usingV2 ? 2 : 1);

    return reinterpret_cast<jlong>(renderer);
}

JNIEXPORT void JNICALL
Java_com_example_vcamtest_MainActivity_nativeRenderFrame(
        JNIEnv* /* env */, jobject /* this */, jlong rendererPtr) {

    auto* renderer = reinterpret_cast<UnifiedRenderer*>(rendererPtr);
    if (!renderer || !renderer->initialized) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    if (renderer->startTimeMs == 0) {
        renderer->startTimeMs = nowMs;
        renderer->lastFpsTime = nowMs;
    }

    renderer->fpsFrameCount++;
    if (nowMs - renderer->lastFpsTime >= 1000) {
        renderer->currentFps = renderer->fpsFrameCount;
        renderer->fpsFrameCount = 0;
        renderer->lastFpsTime = nowMs;
    }

    int64_t timestampMs = nowMs - renderer->startTimeMs;
    int w = renderer->camWidth;
    int h = renderer->camHeight;

    if (renderer->usingV2) {
        // V2 path: render directly into AHardwareBuffer (zero-copy)
        uint8_t* pixels = renderer->writerV2.beginFrame();
        if (pixels) {
            renderTestPattern(pixels, w, h, renderer->frameCount,
                              timestampMs, renderer->currentFps, true);

            // Copy to local frameBuffer for display window
            memcpy(renderer->frameBuffer.data(), pixels, w * h * 4);

            renderer->writerV2.endFrame();
        } else {
            // No buffer available — render to display only
            renderTestPattern(renderer->frameBuffer.data(), w, h,
                              renderer->frameCount, timestampMs,
                              renderer->currentFps, true);
        }

        // Check for format negotiation
        vcam::VirtualCameraWriterV2::FormatRequest req;
        if (renderer->writerV2.checkNegotiation(&req)) {
            LOGI("HAL requested format change: %dx%d format=%d",
                 req.width, req.height, req.format);
        }
    } else {
        // V1 path: render to frame buffer, copy to shared memory
        renderTestPattern(renderer->frameBuffer.data(), w, h,
                          renderer->frameCount, timestampMs,
                          renderer->currentFps, false);
        renderer->writerV1.writeFrame(renderer->frameBuffer.data(),
                                      renderer->frameBuffer.size());
    }

    // Render to display window
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(renderer->window, &buffer, nullptr) == 0) {
        uint8_t* dst = static_cast<uint8_t*>(buffer.bits);
        int dstStride = buffer.stride * 4;
        int srcStride = w * 4;

        for (int y = 0; y < h && y < buffer.height; y++) {
            memcpy(dst + y * dstStride,
                   renderer->frameBuffer.data() + y * srcStride,
                   std::min(srcStride, dstStride));
        }

        ANativeWindow_unlockAndPost(renderer->window);
    }

    renderer->frameCount++;

    if (renderer->frameCount % 60 == 0) {
        LOGI("Rendered %d frames, %d FPS (v%d)",
             renderer->frameCount, renderer->currentFps,
             renderer->usingV2 ? 2 : 1);
    }
}

JNIEXPORT void JNICALL
Java_com_example_vcamtest_MainActivity_nativeDestroyRenderer(
        JNIEnv* /* env */, jobject /* this */, jlong rendererPtr) {

    auto* renderer = reinterpret_cast<UnifiedRenderer*>(rendererPtr);
    if (renderer) {
        if (renderer->usingV2) {
            renderer->writerV2.shutdown();
        } else {
            renderer->writerV1.shutdown();
        }
        if (renderer->window) {
            ANativeWindow_release(renderer->window);
        }
        delete renderer;
        LOGI("Renderer destroyed");
    }
}

}  // extern "C"
