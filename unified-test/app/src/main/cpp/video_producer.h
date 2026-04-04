#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>
#include <android/native_window.h>

class VideoProducer {
public:
    VideoProducer();
    ~VideoProducer();
    
    // Service mode: use fd from VirtualMediaService
    bool startWithFd(int fd, int bufferSize, int headerSize,
                     int width, int height, int fps);
    
    // Legacy mode: connect directly to HAL socket
    bool start(int width = 1280, int height = 720, int fps = 30);
    
    void stop();
    
    bool isRunning() const { return mRunning; }
    
    // Set preview surface - frames will be rendered here too
    void setPreviewSurface(ANativeWindow* window);
    
    // Pattern types
    enum class Pattern {
        COLOR_BARS,
        GRADIENT,
        CHECKERBOARD,
        BOUNCING_BALL
    };
    
    void setPattern(Pattern pattern) { mPattern = pattern; }
    Pattern getPattern() const { return mPattern; }
    
private:
    void producerLoop();
    void producerLoopFd();  // Loop for fd-based mode
    int createAshmem(const char* name, size_t size);
    bool sendFdAndSize(int socket, int fd, uint64_t size);
    
    void generateColorBars(uint8_t* buffer, int width, int height, int frameNum);
    void generateGradient(uint8_t* buffer, int width, int height, int frameNum);
    void generateCheckerboard(uint8_t* buffer, int width, int height, int frameNum);
    void generateBouncingBall(uint8_t* buffer, int width, int height, int frameNum);
    
    // Render frame to preview surface
    void renderToPreview(uint8_t* buffer, int width, int height);
    
    std::mutex mMutex;
    std::atomic<bool> mRunning{false};
    std::thread mProducerThread;
    
    // Legacy mode
    int mSocket = -1;
    int mAshmemFd = -1;
    
    // Common
    void* mMappedBuffer = nullptr;
    size_t mMappedSize = 0;
    int mHeaderSize = 64;
    
    int mWidth = 1280;
    int mHeight = 720;
    int mFps = 30;
    Pattern mPattern = Pattern::COLOR_BARS;
    
    // Mode tracking
    bool mUseFdMode = false;
    
    // Preview surface
    ANativeWindow* mPreviewWindow = nullptr;
    std::mutex mPreviewMutex;
};
