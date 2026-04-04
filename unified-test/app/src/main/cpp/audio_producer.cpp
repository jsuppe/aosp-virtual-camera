/*
 * Audio Producer - Generates audio and sends to Virtual Mic HAL
 * 
 * Two modes:
 * 1. Service mode: Use fd from VirtualMediaService (preferred)
 * 2. Legacy mode: Connect directly to abstract socket @virtual_mic
 */

#include "audio_producer.h"

#include <android/log.h>
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
#include <time.h>

#define LOG_TAG "AudioProducer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Default audio parameters (used in legacy mode)
static constexpr int DEFAULT_SAMPLE_RATE = 48000;
static constexpr int DEFAULT_CHANNELS = 2;
static constexpr int BITS_PER_SAMPLE = 16;

// Buffer parameters
static constexpr size_t BUFFER_FRAMES = 4096;

// Abstract socket name (for legacy mode)
static constexpr const char* SOCKET_NAME = "virtual_mic";

// Audio format enum
enum class AudioFormat : uint32_t {
    PCM_16_BIT = 1,
    PCM_FLOAT = 3,
};

// Audio buffer header (must match HAL and VirtualMediaService)
struct AudioBufferHeader {
    uint32_t magic;              // 0x43494D56 "VMIC"
    uint32_t version;            // 1
    uint32_t sampleRate;
    uint32_t channelCount;
    AudioFormat format;
    uint32_t bytesPerSample;
    uint32_t ringBufferOffset;
    uint32_t ringBufferSize;
    std::atomic<uint32_t> writePos;
    std::atomic<uint32_t> readPos;
    std::atomic<uint64_t> totalSamplesWritten;
    std::atomic<uint64_t> totalSamplesRead;
    std::atomic<uint32_t> flags;
    std::atomic<uint32_t> targetFreqHz;
    std::atomic<uint32_t> targetAmplitude;
    std::atomic<uint64_t> lastWriteTimestampNs;  // For latency measurement
    std::atomic<uint64_t> lastReadTimestampNs;   // Set by consumer
    
    static constexpr uint32_t FLAG_RENDERER_CONNECTED = 0x01;
    static constexpr uint32_t FLAG_ACTIVE = 0x02;
    static constexpr uint32_t FLAG_STOP = 0x04;
};

static constexpr uint32_t VMIC_MAGIC = 0x43494D56;
static constexpr uint32_t VMIC_VERSION = 1;

// Get monotonic timestamp in nanoseconds (for latency measurement)
static inline uint64_t getNowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

AudioProducer::AudioProducer() = default;

AudioProducer::~AudioProducer() {
    stop();
}

// ============================================================================
// Service Mode (fd-based) - Preferred
// ============================================================================

bool AudioProducer::startWithFd(int fd, int bufferSize, int headerSize,
                                int sampleRate, int channels,
                                float frequency, float amplitude) {
    std::lock_guard<std::mutex> lock(mMutex);
    
    if (mRunning) {
        return true;
    }
    
    mUseFdMode = true;
    mSampleRate = sampleRate;
    mChannels = channels;
    mFrequency = frequency;
    mAmplitude = amplitude;
    mHeaderSize = headerSize;
    
    LOGI("Starting with service fd=%d, size=%d, header=%d, rate=%d, ch=%d",
         fd, bufferSize, headerSize, sampleRate, channels);
    
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
    AudioBufferHeader* header = static_cast<AudioBufferHeader*>(mMappedBuffer);
    
    if (header->magic != VMIC_MAGIC) {
        // Initialize header (service may not have done it)
        LOGI("Initializing audio buffer header");
        header->magic = VMIC_MAGIC;
        header->version = VMIC_VERSION;
        header->sampleRate = sampleRate;
        header->channelCount = channels;
        header->format = AudioFormat::PCM_16_BIT;
        header->bytesPerSample = BITS_PER_SAMPLE / 8;
        header->ringBufferOffset = headerSize;
        header->ringBufferSize = bufferSize - headerSize;
        header->writePos.store(0);
        header->readPos.store(0);
        header->totalSamplesWritten.store(0);
        header->totalSamplesRead.store(0);
    }
    
    header->flags.store(AudioBufferHeader::FLAG_RENDERER_CONNECTED | AudioBufferHeader::FLAG_ACTIVE);
    header->targetFreqHz.store(static_cast<uint32_t>(mFrequency));
    header->targetAmplitude.store(static_cast<uint32_t>(mAmplitude * 1000));
    
    LOGI("Audio buffer ready: ringSize=%u, offset=%u",
         header->ringBufferSize, header->ringBufferOffset);
    
    mRunning = true;
    mProducerThread = std::thread(&AudioProducer::producerLoopFd, this);
    
    return true;
}

void AudioProducer::producerLoopFd() {
    AudioBufferHeader* header = static_cast<AudioBufferHeader*>(mMappedBuffer);
    uint8_t* audioData = static_cast<uint8_t*>(mMappedBuffer) + header->ringBufferOffset;
    
    int frameSize = mChannels * (BITS_PER_SAMPLE / 8);
    double phase = 0.0;
    double phaseIncrement = 2.0 * M_PI * mFrequency / mSampleRate;
    
    LOGI("Producer loop (fd mode) started, freq=%.1f Hz, rate=%d, ch=%d",
         mFrequency, mSampleRate, mChannels);
    
    while (mRunning) {
        // Update frequency if changed
        uint32_t targetFreq = header->targetFreqHz.load();
        if (targetFreq > 0 && targetFreq != static_cast<uint32_t>(mFrequency)) {
            mFrequency = targetFreq;
            phaseIncrement = 2.0 * M_PI * mFrequency / mSampleRate;
        }
        
        // Calculate available space
        uint32_t writeBytePos = header->writePos.load(std::memory_order_acquire);
        uint32_t readBytePos = header->readPos.load(std::memory_order_acquire);
        
        size_t availableBytes;
        if (writeBytePos >= readBytePos) {
            availableBytes = header->ringBufferSize - (writeBytePos - readBytePos) - frameSize;
        } else {
            availableBytes = readBytePos - writeBytePos - frameSize;
        }
        
        if (availableBytes < static_cast<size_t>(frameSize)) {
            usleep(1000);  // Buffer full, wait
            continue;
        }
        
        // Generate sample
        int16_t sample = static_cast<int16_t>(mAmplitude * sin(phase) * 32767.0);
        
        // Write frame
        int16_t* framePtr = reinterpret_cast<int16_t*>(audioData + writeBytePos);
        for (int ch = 0; ch < mChannels; ch++) {
            framePtr[ch] = sample;
        }
        
        phase += phaseIncrement;
        if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        
        // Record timestamp for latency measurement
        header->lastWriteTimestampNs.store(getNowNs(), std::memory_order_release);
        
        // Update write position (BYTE offset!)
        uint32_t newWritePos = (writeBytePos + frameSize) % header->ringBufferSize;
        header->writePos.store(newWritePos, std::memory_order_release);
        header->totalSamplesWritten.fetch_add(mChannels);
    }
    
    LOGI("Producer loop (fd mode) ended");
}

// ============================================================================
// Legacy Mode (direct socket) - Fallback
// ============================================================================

bool AudioProducer::start(float frequency, float amplitude) {
    std::lock_guard<std::mutex> lock(mMutex);
    
    if (mRunning) {
        return true;
    }
    
    mUseFdMode = false;
    mSampleRate = DEFAULT_SAMPLE_RATE;
    mChannels = DEFAULT_CHANNELS;
    mFrequency = frequency;
    mAmplitude = amplitude;
    
    int frameSize = mChannels * (BITS_PER_SAMPLE / 8);
    size_t bufferSize = BUFFER_FRAMES * frameSize;
    mHeaderSize = sizeof(AudioBufferHeader);
    
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
    
    // Create ashmem
    size_t totalSize = mHeaderSize + bufferSize;
    mAshmemFd = createAshmem("audio_buffer", totalSize);
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
    AudioBufferHeader* header = static_cast<AudioBufferHeader*>(mMappedBuffer);
    header->magic = VMIC_MAGIC;
    header->version = VMIC_VERSION;
    header->sampleRate = mSampleRate;
    header->channelCount = mChannels;
    header->format = AudioFormat::PCM_16_BIT;
    header->bytesPerSample = BITS_PER_SAMPLE / 8;
    header->ringBufferOffset = mHeaderSize;
    header->ringBufferSize = bufferSize;
    header->writePos.store(0);
    header->readPos.store(0);
    header->totalSamplesWritten.store(0);
    header->totalSamplesRead.store(0);
    header->flags.store(AudioBufferHeader::FLAG_RENDERER_CONNECTED | AudioBufferHeader::FLAG_ACTIVE);
    header->targetFreqHz.store(static_cast<uint32_t>(mFrequency));
    header->targetAmplitude.store(static_cast<uint32_t>(mAmplitude * 1000));
    
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
    
    LOGI("Ashmem sent to HAL, starting audio generation");
    
    mRunning = true;
    mProducerThread = std::thread(&AudioProducer::producerLoop, this);
    
    return true;
}

void AudioProducer::stop() {
    mRunning = false;
    
    if (mProducerThread.joinable()) {
        mProducerThread.join();
    }
    
    if (mMappedBuffer != nullptr) {
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

void AudioProducer::setFrequency(float freq) {
    mFrequency = freq;
    
    if (mMappedBuffer != nullptr) {
        AudioBufferHeader* header = static_cast<AudioBufferHeader*>(mMappedBuffer);
        header->targetFreqHz.store(static_cast<uint32_t>(freq));
    }
}

void AudioProducer::setAmplitude(float amp) {
    mAmplitude = amp;
    
    if (mMappedBuffer != nullptr) {
        AudioBufferHeader* header = static_cast<AudioBufferHeader*>(mMappedBuffer);
        header->targetAmplitude.store(static_cast<uint32_t>(amp * 1000));
    }
}

int AudioProducer::createAshmem(const char* name, size_t size) {
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

bool AudioProducer::sendFdAndSize(int socket, int fd, uint64_t size) {
    // Send fd via SCM_RIGHTS
    char buf[1] = {0};
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };
    
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
    
    // Send size
    if (send(socket, &size, sizeof(size), 0) != sizeof(size)) {
        LOGE("Failed to send size: %s", strerror(errno));
        return false;
    }
    
    return true;
}

void AudioProducer::producerLoop() {
    AudioBufferHeader* header = static_cast<AudioBufferHeader*>(mMappedBuffer);
    uint8_t* audioData = static_cast<uint8_t*>(mMappedBuffer) + header->ringBufferOffset;
    
    int frameSize = mChannels * (BITS_PER_SAMPLE / 8);
    double phase = 0.0;
    double phaseIncrement = 2.0 * M_PI * mFrequency / mSampleRate;
    
    LOGI("Producer loop (legacy) started, freq=%.1f Hz", mFrequency);
    
    while (mRunning) {
        // Update frequency if changed
        uint32_t targetFreq = header->targetFreqHz.load();
        if (targetFreq > 0 && targetFreq != static_cast<uint32_t>(mFrequency)) {
            mFrequency = targetFreq;
            phaseIncrement = 2.0 * M_PI * mFrequency / mSampleRate;
        }
        
        // Calculate available space
        uint32_t writeBytePos = header->writePos.load(std::memory_order_acquire);
        uint32_t readBytePos = header->readPos.load(std::memory_order_acquire);
        
        size_t availableBytes;
        if (writeBytePos >= readBytePos) {
            availableBytes = header->ringBufferSize - (writeBytePos - readBytePos) - frameSize;
        } else {
            availableBytes = readBytePos - writeBytePos - frameSize;
        }
        
        if (availableBytes < static_cast<size_t>(frameSize)) {
            usleep(1000);  // Buffer full, wait
            continue;
        }
        
        // Write one frame
        int16_t* framePtr = reinterpret_cast<int16_t*>(audioData + writeBytePos);
        
        int16_t sample = static_cast<int16_t>(mAmplitude * sin(phase) * 32767.0);
        framePtr[0] = sample;  // Left
        framePtr[1] = sample;  // Right
        
        phase += phaseIncrement;
        if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        
        // Record timestamp for latency measurement
        header->lastWriteTimestampNs.store(getNowNs(), std::memory_order_release);
        
        // Update write position (BYTE offset!)
        uint32_t newWritePos = (writeBytePos + frameSize) % header->ringBufferSize;
        header->writePos.store(newWritePos, std::memory_order_release);
        header->totalSamplesWritten.fetch_add(mChannels);
    }
    
    LOGI("Producer loop (legacy) ended");
}
