/*
 * VirtualCameraProvider - Implementation
 * 
 * Connects to VirtualCameraService via IVirtualCameraManager AIDL.
 * No JNI required - pure Binder IPC.
 * 
 * Location: hardware/interfaces/camera/provider/virtual/
 */
#define LOG_TAG "VirtualCameraProvider"

#include "VirtualCameraProvider.h"
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <chrono>

namespace aidl::android::hardware::camera::provider::implementation {

using ::aidl::android::hardware::camera::virtual_::IVirtualCameraManager;

VirtualCameraProvider::VirtualCameraProvider() {
    LOG(INFO) << "VirtualCameraProvider created";
    connectToService();
}

VirtualCameraProvider::~VirtualCameraProvider() {
    mPollingRunning = false;
    if (mPollingThread.joinable()) {
        mPollingThread.join();
    }
    LOG(INFO) << "VirtualCameraProvider destroyed";
}

void VirtualCameraProvider::connectToService() {
    // Get the VirtualCameraManager service
    const std::string serviceName = "virtual_camera_manager";
    
    ndk::SpAIBinder binder(AServiceManager_checkService(serviceName.c_str()));
    if (binder.get() == nullptr) {
        LOG(WARNING) << "VirtualCameraManager service not available yet";
        return;
    }
    
    mManager = IVirtualCameraManager::fromBinder(binder);
    if (mManager == nullptr) {
        LOG(ERROR) << "Failed to get IVirtualCameraManager interface";
        return;
    }
    
    LOG(INFO) << "Connected to VirtualCameraManager service";
    
    // Start polling for camera changes
    mPollingRunning = true;
    mPollingThread = std::thread(&VirtualCameraProvider::pollingLoop, this);
}

void VirtualCameraProvider::pollingLoop() {
    // Poll for camera registration changes
    // In production, you'd use a callback/notification mechanism instead
    while (mPollingRunning) {
        refreshCameras();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void VirtualCameraProvider::refreshCameras() {
    if (!mManager) {
        connectToService();
        if (!mManager) return;
    }
    
    std::vector<int32_t> cameraIds;
    auto status = mManager->getRegisteredCameraIds(&cameraIds);
    if (!status.isOk()) {
        LOG(ERROR) << "Failed to get camera IDs: " << status.getDescription();
        return;
    }
    
    std::lock_guard<std::mutex> lock(mMutex);
    
    // Check for new cameras
    for (int32_t id : cameraIds) {
        if (mDevices.find(id) == mDevices.end()) {
            // New camera - get config and create device
            VirtualCameraConfig config;
            auto configStatus = mManager->getCameraConfig(id, &config);
            if (!configStatus.isOk()) {
                LOG(ERROR) << "Failed to get config for camera " << id;
                continue;
            }
            
            LOG(INFO) << "Discovered new virtual camera: id=" << id 
                      << " name=" << config.name
                      << " resolution=" << config.maxWidth << "x" << config.maxHeight;
            
            auto device = ndk::SharedRefBase::make<VirtualCameraDevice>(
                id, config, this);
            mDevices[id] = device;
            
            // Notify framework
            if (mCallback) {
                mCallback->cameraDeviceStatusChange(
                    makeDeviceName(id), 
                    CameraDeviceStatus::PRESENT);
            }
        }
    }
    
    // Check for removed cameras
    std::vector<int> toRemove;
    for (const auto& [id, device] : mDevices) {
        bool found = false;
        for (int32_t registeredId : cameraIds) {
            if (registeredId == id) {
                found = true;
                break;
            }
        }
        if (!found) {
            toRemove.push_back(id);
        }
    }
    
    for (int id : toRemove) {
        LOG(INFO) << "Virtual camera removed: " << id;
        
        if (mCallback) {
            mCallback->cameraDeviceStatusChange(
                makeDeviceName(id), 
                CameraDeviceStatus::NOT_PRESENT);
        }
        
        mDevices.erase(id);
    }
}

std::string VirtualCameraProvider::makeDeviceName(int cameraId) const {
    return "device@1.0/virtual/" + std::to_string(cameraId);
}

ndk::ScopedAStatus VirtualCameraProvider::setCallback(
        const std::shared_ptr<ICameraProviderCallback>& callback) {
    std::lock_guard<std::mutex> lock(mMutex);
    mCallback = callback;
    
    // Notify existing cameras
    for (const auto& [id, device] : mDevices) {
        if (mCallback) {
            mCallback->cameraDeviceStatusChange(
                makeDeviceName(id), 
                CameraDeviceStatus::PRESENT);
        }
    }
    
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraProvider::getVendorTags(
        std::vector<VendorTagSection>* vendorTags) {
    vendorTags->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraProvider::getCameraIdList(
        std::vector<std::string>* cameraIds) {
    std::lock_guard<std::mutex> lock(mMutex);
    
    cameraIds->clear();
    for (const auto& [id, device] : mDevices) {
        cameraIds->push_back(makeDeviceName(id));
    }
    
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraProvider::getCameraDeviceInterface(
        const std::string& cameraId,
        std::shared_ptr<ICameraDevice>* device) {
    std::lock_guard<std::mutex> lock(mMutex);
    
    // Parse camera ID from device name: "device@1.0/virtual/1000"
    size_t lastSlash = cameraId.rfind('/');
    if (lastSlash == std::string::npos) {
        return ndk::ScopedAStatus::fromServiceSpecificError(
            static_cast<int32_t>(Status::ILLEGAL_ARGUMENT));
    }
    
    int id = std::stoi(cameraId.substr(lastSlash + 1));
    
    auto it = mDevices.find(id);
    if (it == mDevices.end()) {
        return ndk::ScopedAStatus::fromServiceSpecificError(
            static_cast<int32_t>(Status::ILLEGAL_ARGUMENT));
    }
    
    *device = it->second;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraProvider::notifyDeviceStateChange(int64_t state) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraProvider::getConcurrentCameraIds(
        std::vector<ConcurrentCameraIdCombination>* combinations) {
    combinations->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraProvider::isConcurrentStreamCombinationSupported(
        const std::vector<CameraIdAndStreamCombination>& configs,
        bool* supported) {
    *supported = true;
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::camera::provider::implementation
