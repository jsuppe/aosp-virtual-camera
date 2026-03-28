/*
 * VirtualCameraDevice - Individual camera device
 * 
 * Implements ICameraDevice for a single virtual camera.
 * Uses IVirtualCameraManager for buffer operations.
 * 
 * Location: hardware/interfaces/camera/provider/virtual/
 */
#pragma once

#include <aidl/android/hardware/camera/device/BnCameraDevice.h>
#include <aidl/android/hardware/camera/device/BnCameraDeviceSession.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/virtual/IVirtualCameraManager.h>
#include <aidl/android/hardware/camera/virtual/VirtualCameraConfig.h>
#include <aidl/android/hardware/camera/virtual/StreamConfig.h>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <queue>

namespace aidl::android::hardware::camera::provider::implementation {

using namespace aidl::android::hardware::camera::device;
using ::aidl::android::hardware::camera::virtual_::IVirtualCameraManager;
using ::aidl::android::hardware::camera::virtual_::VirtualCameraConfig;
using ::aidl::android::hardware::camera::virtual_::StreamConfig;

// Forward declaration
class VirtualCameraProvider;

class VirtualCameraDevice : public BnCameraDevice {
public:
    VirtualCameraDevice(int cameraId, 
                        const VirtualCameraConfig& config,
                        VirtualCameraProvider* provider);
    ~VirtualCameraDevice() override;

    // ICameraDevice interface
    ndk::ScopedAStatus getCameraCharacteristics(
        CameraMetadata* characteristics) override;
    
    ndk::ScopedAStatus getPhysicalCameraCharacteristics(
        const std::string& physicalCameraId,
        CameraMetadata* characteristics) override;
    
    ndk::ScopedAStatus getResourceCost(
        CameraResourceCost* resourceCost) override;
    
    ndk::ScopedAStatus isStreamCombinationSupported(
        const StreamConfiguration& streams,
        bool* supported) override;
    
    ndk::ScopedAStatus open(
        const std::shared_ptr<ICameraDeviceCallback>& callback,
        std::shared_ptr<ICameraDeviceSession>* session) override;
    
    ndk::ScopedAStatus openInjectionSession(
        const std::shared_ptr<ICameraDeviceCallback>& callback,
        std::shared_ptr<ICameraInjectionSession>* session) override;
    
    ndk::ScopedAStatus setTorchMode(bool on) override;
    ndk::ScopedAStatus turnOnTorchWithStrengthLevel(int32_t strength) override;
    ndk::ScopedAStatus getTorchStrengthLevel(int32_t* strength) override;

    int getCameraId() const { return mCameraId; }
    const VirtualCameraConfig& getConfig() const { return mConfig; }
    VirtualCameraProvider* getProvider() const { return mProvider; }

private:
    const int mCameraId;
    VirtualCameraConfig mConfig;
    VirtualCameraProvider* mProvider;
    CameraMetadata mCharacteristics;
    
    void buildCharacteristics();
};

/**
 * Camera session - handles capture requests
 */
class VirtualCameraSession : public BnCameraDeviceSession {
public:
    VirtualCameraSession(VirtualCameraDevice* device,
                         std::shared_ptr<ICameraDeviceCallback> callback);
    ~VirtualCameraSession() override;

    // ICameraDeviceSession interface
    ndk::ScopedAStatus close() override;
    
    ndk::ScopedAStatus configureStreams(
        const StreamConfiguration& config,
        std::vector<HalStream>* halStreams) override;
    
    ndk::ScopedAStatus constructDefaultRequestSettings(
        RequestTemplate type,
        CameraMetadata* settings) override;
    
    ndk::ScopedAStatus processCaptureRequest(
        const std::vector<CaptureRequest>& requests,
        const std::vector<BufferCache>& cachesToRemove,
        int32_t* numRequestProcessed) override;
    
    ndk::ScopedAStatus signalStreamFlush(
        const std::vector<int32_t>& streamIds,
        int32_t streamConfigCounter) override;
    
    ndk::ScopedAStatus flush() override;
    
    ndk::ScopedAStatus switchToOffline(
        const std::vector<int32_t>& streamsToKeep,
        CameraOfflineSessionInfo* offlineSessionInfo,
        std::shared_ptr<ICameraOfflineSession>* offlineSession) override;
    
    ndk::ScopedAStatus repeatingRequestEnd(
        int32_t frameNumber,
        const std::vector<int32_t>& streamIds) override;

private:
    VirtualCameraDevice* mDevice;
    std::shared_ptr<ICameraDeviceCallback> mCallback;
    std::shared_ptr<IVirtualCameraManager> mManager;
    
    std::mutex mMutex;
    bool mClosed = false;
    
    // Stream configuration
    std::vector<StreamConfig> mStreams;
    
    // Request processing
    std::thread mRequestThread;
    std::condition_variable mRequestCv;
    std::queue<CaptureRequest> mPendingRequests;
    bool mRequestThreadRunning = false;
    
    void requestThreadLoop();
    void processRequest(const CaptureRequest& request);
};

}  // namespace aidl::android::hardware::camera::provider::implementation
