/*
 * Video Producer - Generates video frames and sends to Virtual Camera HAL
 * 
 * Two modes:
 * 1. Service mode: Use fd from VirtualMediaService (preferred)
 * 2. Legacy mode: Connect directly to abstract socket @virtual_camera
 */

#include "video_producer.h"

#include <android/log.h>
#include <android/native_window.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cmath>
#include <cerrno>
#include <fcntl.h>
#include <linux/ashmem.h>
#include <sys/ioctl.h>
#include <chrono>
#include <time.h>

#define LOG_TAG "VideoProducer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Abstract socket name (for legacy mode)
static constexpr const char* SOCKET_NAME = "virtual_camera";

// Frame header (must match HAL's FrameHeader exactly!)
struct FrameHeader {
    std::atomic<uint32_t> magic;        // 0x56434D46 "VCMF"
    std::atomic<uint32_t> version;      // 1
    std::atomic<uint32_t> width;
    std::atomic<uint32_t> height;
    std::atomic<uint32_t> format;       // RGBA8888 = 1
    std::atomic<uint32_t> stride;       // Bytes per row
    std::atomic<uint64_t> frameNumber;  // Frame counter
    std::atomic<uint64_t> timestamp;    // Frame timestamp ns
    std::atomic<uint32_t> dataOffset;   // Offset to frame data
    std::atomic<uint32_t> dataSize;     // Size of frame data
    std::atomic<uint32_t> flags;        // RENDERER_ACTIVE = 2
    
    static constexpr uint32_t FLAG_RENDERER_ACTIVE = 0x02;  // Must be 2 to match HAL
    static constexpr uint32_t FLAG_FRAME_READY = 0x04;     // Frame is complete, safe to read
};

static constexpr uint32_t VCMF_MAGIC = 0x56434D46;  // "VCMF" - must match HAL
static constexpr uint32_t VCMF_VERSION = 1;
static constexpr uint32_t FORMAT_RGBA8888 = 1;

VideoProducer::VideoProducer() = default;

VideoProducer::~VideoProducer() {
    stop();
    setPreviewSurface(nullptr);
}

void VideoProducer::setPreviewSurface(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(mPreviewMutex);
    if (mPreviewWindow) {
        ANativeWindow_release(mPreviewWindow);
    }
    mPreviewWindow = window;
    if (mPreviewWindow) {
        ANativeWindow_acquire(mPreviewWindow);
        LOGI("Preview surface set");
    }
}

void VideoProducer::renderToPreview(uint8_t* buffer, int width, int height) {
    std::lock_guard<std::mutex> lock(mPreviewMutex);
    if (!mPreviewWindow) return;
    
    ANativeWindow_Buffer windowBuffer;
    if (ANativeWindow_lock(mPreviewWindow, &windowBuffer, nullptr) != 0) {
        return;
    }
    
    // Copy RGBA data to preview surface
    int srcStride = width * 4;
    int dstStride = windowBuffer.stride * 4;  // Assuming RGBA format
    
    // Scale if dimensions don't match
    if (windowBuffer.width == width && windowBuffer.height == height) {
        // Direct copy
        for (int y = 0; y < height; y++) {
            memcpy((uint8_t*)windowBuffer.bits + y * dstStride,
                   buffer + y * srcStride,
                   srcStride);
        }
    } else {
        // Simple nearest-neighbor scaling
        float scaleX = (float)width / windowBuffer.width;
        float scaleY = (float)height / windowBuffer.height;
        
        for (int dy = 0; dy < windowBuffer.height; dy++) {
            int sy = (int)(dy * scaleY);
            if (sy >= height) sy = height - 1;
            
            uint32_t* dstRow = (uint32_t*)((uint8_t*)windowBuffer.bits + dy * dstStride);
            uint32_t* srcRow = (uint32_t*)(buffer + sy * srcStride);
            
            for (int dx = 0; dx < windowBuffer.width; dx++) {
                int sx = (int)(dx * scaleX);
                if (sx >= width) sx = width - 1;
                dstRow[dx] = srcRow[sx];
            }
        }
    }
    
    ANativeWindow_unlockAndPost(mPreviewWindow);
}

// ============================================================================
// Service Mode (fd-based) - Preferred
// ============================================================================

bool VideoProducer::startWithFd(int fd, int bufferSize, int headerSize,
                                int width, int height, int fps) {
    std::lock_guard<std::mutex> lock(mMutex);
    
    if (mRunning) {
        return true;
    }
    
    mUseFdMode = true;
    mWidth = width;
    mHeight = height;
    mFps = fps;
    mHeaderSize = headerSize;
    
    LOGI("Starting with service fd=%d, size=%d, header=%d, %dx%d @ %dfps",
         fd, bufferSize, headerSize, width, height, fps);
    
    // Dup the fd so we own it
    int ourFd = dup(fd);
    if (ourFd < 0) {
        LOGE("Failed to dup fd: %s", strerror(errno));
        return false;
    }
    mAshmemFd = ourFd;
    
    // Map the buffer
    mMappedBuffer = mmap(nullptr, bufferSize, PROT_READ | PROT_WRITE, 
                         MAP_SHARED, mAshmemFd, 0);
    if (mMappedBuffer == MAP_FAILED) {
        LOGE("Failed to mmap: %s", strerror(errno));
        close(mAshmemFd);
        mAshmemFd = -1;
        mMappedBuffer = nullptr;
        return false;
    }
    mMappedSize = bufferSize;
    
    // Validate or initialize header
    FrameHeader* header = static_cast<FrameHeader*>(mMappedBuffer);
    
    size_t frameDataSize = width * height * 4;  // RGBA8888
    
    if (header->magic.load() != VCMF_MAGIC) {
        // Initialize header (service may not have done it)
        LOGI("Initializing video frame header");
        header->magic.store(VCMF_MAGIC);
        header->version.store(VCMF_VERSION);
        header->width.store(width);
        header->height.store(height);
        header->format.store(FORMAT_RGBA8888);
        header->stride.store(width * 4);  // RGBA = 4 bytes per pixel
        header->frameNumber.store(0);
        header->timestamp.store(0);
        header->dataOffset.store(headerSize);
        header->dataSize.store(frameDataSize);
    }
    
    header->flags.store(FrameHeader::FLAG_RENDERER_ACTIVE);
    
    LOGI("Video buffer ready: dataSize=%zu, offset=%d",
         frameDataSize, headerSize);
    
    mRunning = true;
    mProducerThread = std::thread(&VideoProducer::producerLoopFd, this);
    
    return true;
}

void VideoProducer::producerLoopFd() {
    FrameHeader* header = static_cast<FrameHeader*>(mMappedBuffer);
    uint8_t* frameData = static_cast<uint8_t*>(mMappedBuffer) + mHeaderSize;
    
    int frameNum = 0;
    auto frameInterval = std::chrono::microseconds(1000000 / mFps);
    auto nextFrame = std::chrono::steady_clock::now();
    
    LOGI("Producer loop (fd mode) started, %dx%d @ %dfps", mWidth, mHeight, mFps);
    
    while (mRunning) {
        // Clear FRAME_READY before writing (tell HAL we're writing)
        uint32_t flags = header->flags.load(std::memory_order_acquire);
        header->flags.store(flags & ~FrameHeader::FLAG_FRAME_READY, std::memory_order_release);
        
        // Memory barrier to ensure flag is cleared before we start writing
        std::atomic_thread_fence(std::memory_order_seq_cst);
        
        // Generate frame based on pattern
        switch (mPattern) {
            case Pattern::COLOR_BARS:
                generateColorBars(frameData, mWidth, mHeight, frameNum);
                break;
            case Pattern::GRADIENT:
                generateGradient(frameData, mWidth, mHeight, frameNum);
                break;
            case Pattern::CHECKERBOARD:
                generateCheckerboard(frameData, mWidth, mHeight, frameNum);
                break;
            case Pattern::BOUNCING_BALL:
                generateBouncingBall(frameData, mWidth, mHeight, frameNum);
                break;
        }
        
        // Update timestamp using CLOCK_BOOTTIME (matches Camera2 timestamps)
        struct timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        header->timestamp.store(ns, std::memory_order_release);
        header->frameNumber.store(frameNum, std::memory_order_release);
        
        // Memory barrier to ensure all writes complete
        std::atomic_thread_fence(std::memory_order_seq_cst);
        
        // Set FRAME_READY after writing complete (tell HAL frame is safe to read)
        flags = header->flags.load(std::memory_order_acquire);
        header->flags.store(flags | FrameHeader::FLAG_FRAME_READY, std::memory_order_release);
        
        // Also render to preview surface (left side of unified test)
        renderToPreview(frameData, mWidth, mHeight);
        
        frameNum++;
        
        // Wait for next frame time
        nextFrame += frameInterval;
        std::this_thread::sleep_until(nextFrame);
    }
    
    LOGI("Producer loop (fd mode) ended, %d frames generated", frameNum);
}

// ============================================================================
// Legacy Mode (direct socket) - Fallback
// ============================================================================

bool VideoProducer::start(int width, int height, int fps) {
    std::lock_guard<std::mutex> lock(mMutex);
    
    if (mRunning) {
        return true;
    }
    
    mUseFdMode = false;
    mWidth = width;
    mHeight = height;
    mFps = fps;
    mHeaderSize = sizeof(FrameHeader);
    
    // Create socket
    mSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mSocket < 0) {
        LOGE("Failed to create socket: %s", strerror(errno));
        return false;
    }
    
    // Connect to abstract socket
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';  // Abstract socket marker
    strncpy(addr.sun_path + 1, SOCKET_NAME, sizeof(addr.sun_path) - 2);
    
    socklen_t addrLen = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SOCKET_NAME);
    
    LOGI("Connecting to abstract socket @%s...", SOCKET_NAME);
    if (connect(mSocket, (struct sockaddr*)&addr, addrLen) < 0) {
        LOGE("Failed to connect: %s", strerror(errno));
        close(mSocket);
        mSocket = -1;
        return false;
    }
    LOGI("Connected!");
    
    // Calculate buffer size: header + frame data (RGBA = 4 bytes per pixel)
    size_t frameDataSize = mWidth * mHeight * 4;
    size_t totalSize = mHeaderSize + frameDataSize;
    
    // Create ashmem
    mAshmemFd = createAshmem("video_frame", totalSize);
    if (mAshmemFd < 0) {
        close(mSocket);
        mSocket = -1;
        return false;
    }
    
    // Map the buffer
    mMappedBuffer = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, 
                         MAP_SHARED, mAshmemFd, 0);
    if (mMappedBuffer == MAP_FAILED) {
        LOGE("Failed to mmap: %s", strerror(errno));
        close(mAshmemFd);
        close(mSocket);
        mAshmemFd = -1;
        mSocket = -1;
        return false;
    }
    mMappedSize = totalSize;
    
    // Initialize header
    FrameHeader* header = static_cast<FrameHeader*>(mMappedBuffer);
    header->magic.store(VCMF_MAGIC);
    header->version.store(VCMF_VERSION);
    header->width.store(mWidth);
    header->height.store(mHeight);
    header->format.store(FORMAT_RGBA8888);
    header->stride.store(mWidth * 4);  // RGBA = 4 bytes per pixel
    header->frameNumber.store(0);
    header->timestamp.store(0);
    header->dataOffset.store(sizeof(FrameHeader));
    header->dataSize.store(frameDataSize);
    header->flags.store(FrameHeader::FLAG_RENDERER_ACTIVE);
    
    // Send fd and size to HAL
    if (!sendFdAndSize(mSocket, mAshmemFd, totalSize)) {
        munmap(mMappedBuffer, mMappedSize);
        close(mAshmemFd);
        close(mSocket);
        mMappedBuffer = nullptr;
        mAshmemFd = -1;
        mSocket = -1;
        return false;
    }
    
    LOGI("Ashmem sent to HAL, starting video generation (%dx%d @ %d fps)", 
         mWidth, mHeight, mFps);
    
    mRunning = true;
    mProducerThread = std::thread(&VideoProducer::producerLoop, this);
    
    return true;
}

void VideoProducer::stop() {
    mRunning = false;
    
    if (mProducerThread.joinable()) {
        mProducerThread.join();
    }
    
    if (mMappedBuffer != nullptr) {
        // Clear renderer active flag
        FrameHeader* header = static_cast<FrameHeader*>(mMappedBuffer);
        header->flags.store(0);
        
        munmap(mMappedBuffer, mMappedSize);
        mMappedBuffer = nullptr;
    }
    
    if (mAshmemFd >= 0) {
        close(mAshmemFd);
        mAshmemFd = -1;
    }
    
    if (mSocket >= 0) {
        close(mSocket);
        mSocket = -1;
    }
    
    mUseFdMode = false;
}

int VideoProducer::createAshmem(const char* name, size_t size) {
    int fd = open("/dev/ashmem", O_RDWR);
    if (fd < 0) {
        LOGE("Failed to open /dev/ashmem: %s", strerror(errno));
        return -1;
    }
    
    if (ioctl(fd, ASHMEM_SET_NAME, name) < 0) {
        LOGE("Failed to set ashmem name: %s", strerror(errno));
        close(fd);
        return -1;
    }
    
    if (ioctl(fd, ASHMEM_SET_SIZE, size) < 0) {
        LOGE("Failed to set ashmem size: %s", strerror(errno));
        close(fd);
        return -1;
    }
    
    return fd;
}

bool VideoProducer::sendFdAndSize(int socket, int fd, uint64_t size) {
    // Send size + fd via SCM_RIGHTS (camera HAL expects size first)
    struct iovec iov;
    iov.iov_base = &size;
    iov.iov_len = sizeof(size);
    
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    memset(cmsgbuf, 0, sizeof(cmsgbuf));
    
    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int*)CMSG_DATA(cmsg)) = fd;
    
    if (sendmsg(socket, &msg, 0) < 0) {
        LOGE("Failed to send fd: %s", strerror(errno));
        return false;
    }
    
    return true;
}

void VideoProducer::producerLoop() {
    FrameHeader* header = static_cast<FrameHeader*>(mMappedBuffer);
    uint8_t* frameData = static_cast<uint8_t*>(mMappedBuffer) + sizeof(FrameHeader);
    
    int frameNum = 0;
    auto frameInterval = std::chrono::microseconds(1000000 / mFps);
    auto nextFrame = std::chrono::steady_clock::now();
    
    LOGI("Producer loop (legacy) started");
    
    while (mRunning) {
        // Generate frame based on pattern
        switch (mPattern) {
            case Pattern::COLOR_BARS:
                generateColorBars(frameData, mWidth, mHeight, frameNum);
                break;
            case Pattern::GRADIENT:
                generateGradient(frameData, mWidth, mHeight, frameNum);
                break;
            case Pattern::CHECKERBOARD:
                generateCheckerboard(frameData, mWidth, mHeight, frameNum);
                break;
            case Pattern::BOUNCING_BALL:
                generateBouncingBall(frameData, mWidth, mHeight, frameNum);
                break;
        }
        
        // Update timestamp
        auto now = std::chrono::system_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        header->timestamp.store(ns);
        
        frameNum++;
        
        // Wait for next frame time
        nextFrame += frameInterval;
        std::this_thread::sleep_until(nextFrame);
    }
    
    LOGI("Producer loop (legacy) ended, %d frames generated", frameNum);
}

// ============================================================================
// Pattern Generators
// ============================================================================

void VideoProducer::generateColorBars(uint8_t* buffer, int width, int height, int frameNum) {
    // SMPTE color bars: White, Yellow, Cyan, Green, Magenta, Red, Blue, Black
    static const uint8_t bars[8][3] = {
        {255, 255, 255},  // White
        {255, 255, 0},    // Yellow
        {0, 255, 255},    // Cyan
        {0, 255, 0},      // Green
        {255, 0, 255},    // Magenta
        {255, 0, 0},      // Red
        {0, 0, 255},      // Blue
        {0, 0, 0}         // Black
    };
    
    int barWidth = width / 8;
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int barIndex = x / barWidth;
            if (barIndex >= 8) barIndex = 7;
            
            int offset = (y * width + x) * 4;
            buffer[offset + 0] = bars[barIndex][0];  // R
            buffer[offset + 1] = bars[barIndex][1];  // G
            buffer[offset + 2] = bars[barIndex][2];  // B
            buffer[offset + 3] = 255;                 // A
        }
    }
}

void VideoProducer::generateGradient(uint8_t* buffer, int width, int height, int frameNum) {
    float hueOffset = (frameNum * 2) % 360;
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // HSV to RGB with shifting hue
            float h = fmod((float)x / width * 360.0f + hueOffset, 360.0f);
            float s = 1.0f;
            float v = (float)y / height;
            
            float c = v * s;
            float hp = h / 60.0f;
            float x2 = c * (1 - fabs(fmod(hp, 2) - 1));
            
            float r1, g1, b1;
            if (hp < 1) { r1 = c; g1 = x2; b1 = 0; }
            else if (hp < 2) { r1 = x2; g1 = c; b1 = 0; }
            else if (hp < 3) { r1 = 0; g1 = c; b1 = x2; }
            else if (hp < 4) { r1 = 0; g1 = x2; b1 = c; }
            else if (hp < 5) { r1 = x2; g1 = 0; b1 = c; }
            else { r1 = c; g1 = 0; b1 = x2; }
            
            float m = v - c;
            
            int offset = (y * width + x) * 4;
            buffer[offset + 0] = (uint8_t)((r1 + m) * 255);
            buffer[offset + 1] = (uint8_t)((g1 + m) * 255);
            buffer[offset + 2] = (uint8_t)((b1 + m) * 255);
            buffer[offset + 3] = 255;
        }
    }
}

void VideoProducer::generateCheckerboard(uint8_t* buffer, int width, int height, int frameNum) {
    int squareSize = 64;
    int offsetX = (frameNum * 2) % (squareSize * 2);
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int checkX = (x + offsetX) / squareSize;
            int checkY = y / squareSize;
            bool isWhite = (checkX + checkY) % 2 == 0;
            
            int offset = (y * width + x) * 4;
            uint8_t color = isWhite ? 255 : 0;
            buffer[offset + 0] = color;
            buffer[offset + 1] = color;
            buffer[offset + 2] = color;
            buffer[offset + 3] = 255;
        }
    }
}

void VideoProducer::generateBouncingBall(uint8_t* buffer, int width, int height, int frameNum) {
    // Background
    memset(buffer, 32, width * height * 4);  // Dark gray
    for (int i = 3; i < width * height * 4; i += 4) {
        buffer[i] = 255;  // Alpha
    }
    
    // Ball parameters
    int ballRadius = 50;
    float angle = frameNum * 0.05f;
    
    int ballX = width / 2 + (int)(cos(angle) * (width / 3));
    int ballY = height / 2 + (int)(sin(angle * 1.3f) * (height / 3));
    
    // Draw ball
    for (int y = ballY - ballRadius; y <= ballY + ballRadius; y++) {
        for (int x = ballX - ballRadius; x <= ballX + ballRadius; x++) {
            if (x < 0 || x >= width || y < 0 || y >= height) continue;
            
            int dx = x - ballX;
            int dy = y - ballY;
            if (dx * dx + dy * dy <= ballRadius * ballRadius) {
                int offset = (y * width + x) * 4;
                buffer[offset + 0] = 255;  // Red ball
                buffer[offset + 1] = 100;
                buffer[offset + 2] = 100;
                buffer[offset + 3] = 255;
            }
        }
    }
}
