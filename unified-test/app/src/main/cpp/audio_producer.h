#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>

class AudioProducer {
public:
    AudioProducer();
    ~AudioProducer();
    
    // Service mode: use fd from VirtualMediaService
    bool startWithFd(int fd, int bufferSize, int headerSize,
                     int sampleRate, int channels,
                     float frequency = 440.0f, float amplitude = 0.5f);
    
    // Legacy mode: connect directly to HAL socket
    bool start(float frequency = 440.0f, float amplitude = 0.5f);
    
    void stop();
    
    bool isRunning() const { return mRunning; }
    
    void setFrequency(float freq);
    void setAmplitude(float amp);
    
    float getFrequency() const { return mFrequency; }
    float getAmplitude() const { return mAmplitude; }
    
private:
    void producerLoop();
    void producerLoopFd();  // Loop for fd-based mode
    int createAshmem(const char* name, size_t size);
    bool sendFdAndSize(int socket, int fd, uint64_t size);
    
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
    
    // Configuration
    int mSampleRate = 48000;
    int mChannels = 1;
    float mFrequency = 440.0f;
    float mAmplitude = 0.5f;
    
    // Mode tracking
    bool mUseFdMode = false;
};
