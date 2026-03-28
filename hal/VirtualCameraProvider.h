/*
 * VirtualCameraProvider - Camera HAL Provider
 * 
 * Implements ICameraProvider to expose virtual cameras to Android framework.
 * Communicates with VirtualCameraService via IVirtualCameraManager AIDL.
 * 
 * Location: hardware/interfaces/camera/provider/virtual/
 */
#pragma once

#include <aidl/android/hardware/camera/provider/BnCameraProvider.h>
#include <aidl/android/hardware/camera/provider/ICameraProviderCallback.h>
#include <aidl/android/hardware/camera/virtual/IVirtualCameraManager.h>
#include <mutex>
#include <unordered_map>
#include <thread>

#include "VirtualCameraDevice.h"

namespace aidl::android::hardware::camera::provider::implementation {

using ::aidl::android::hardware::camera::virtual_::IVirtualCameraManager;
using ::aidl::android::hardware::camera::virtual_::VirtualCameraConfig;

class VirtualCameraProvider : public BnCameraProvider {
public:
    VirtualCameraProvider();
    ~VirtualCameraProvider() override;

    // ICameraProvider interface
    ndk::ScopedAStatus setCallback(
        const std::shared_ptr<ICameraProviderCallback>& callback) override;
    
    ndk::ScopedAStatus getVendorTags(
        std::vector<VendorTagSection>* vendorTags) override;
    
    ndk::ScopedAStatus getCameraIdList(
        std::vector<std::string>* cameraIds) override;
    
    ndk::ScopedAStatus getCameraDeviceInterface(
        const std::string& cameraId,
        std::shared_ptr<ICameraDevice>* device) override;
    
    ndk::ScopedAStatus notifyDeviceStateChange(int64_t state) override;
    
    ndk::ScopedAStatus getConcurrentCameraIds(
        std::vector<ConcurrentCameraIdCombination>* combinations) override;
    
    ndk::ScopedAStatus isConcurrentStreamCombinationSupported(
        const std::vector<CameraIdAndStreamCombination>& configs,
        bool* supported) override;
    
    // Access to manager for device/session use
    std::shared_ptr<IVirtualCameraManager> getManager() { return mManager; }

private:
    std::mutex mMutex;
    std::shared_ptr<ICameraProviderCallback> mCallback;
    std::shared_ptr<IVirtualCameraManager> mManager;
    std::unordered_map<int, std::shared_ptr<VirtualCameraDevice>> mDevices;
    
    // Background thread to poll for camera changes
    std::thread mPollingThread;
    std::atomic<bool> mPollingRunning{false};
    
    void connectToService();
    void pollingLoop();
    void refreshCameras();
    std::string makeDeviceName(int cameraId) const;
};

}  // namespace aidl::android::hardware::camera::provider::implementation
