/*
 * VirtualCameraProvider - Stub HAL Provider
 * 
 * Minimal implementation to register a virtual camera.
 */
#pragma once

#include <aidl/android/hardware/camera/provider/BnCameraProvider.h>
#include <aidl/android/hardware/camera/provider/ICameraProviderCallback.h>
#include <mutex>
#include <memory>

namespace aidl::android::hardware::camera::provider::implementation {

// Forward declarations
class VirtualCameraFrameSource;
class VirtualCameraFrameSourceV2;

class VirtualCameraProvider : public BnCameraProvider {
public:
    VirtualCameraProvider();
    ~VirtualCameraProvider() override;

    // Get the shared FrameSource (for sessions to use)
    std::shared_ptr<VirtualCameraFrameSource> getFrameSource() const { return mFrameSource; }

    // Get the v2 zero-copy frame source (for sessions to use)
    std::shared_ptr<VirtualCameraFrameSourceV2> getFrameSourceV2() const { return mFrameSourceV2; }

    // ICameraProvider interface
    ndk::ScopedAStatus setCallback(
            const std::shared_ptr<ICameraProviderCallback>& callback) override;
    
    ndk::ScopedAStatus getVendorTags(
            std::vector<common::VendorTagSection>* vendorTags) override;
    
    ndk::ScopedAStatus getCameraIdList(
            std::vector<std::string>* cameraIds) override;
    
    ndk::ScopedAStatus getCameraDeviceInterface(
            const std::string& cameraDeviceId,
            std::shared_ptr<device::ICameraDevice>* device) override;
    
    ndk::ScopedAStatus notifyDeviceStateChange(int64_t deviceState) override;
    
    ndk::ScopedAStatus getConcurrentCameraIds(
            std::vector<ConcurrentCameraIdCombination>* concurrentCameraIds) override;
    
    ndk::ScopedAStatus isConcurrentStreamCombinationSupported(
            const std::vector<CameraIdAndStreamCombination>& configs,
            bool* supported) override;

private:
    std::mutex mLock;
    std::shared_ptr<ICameraProviderCallback> mCallback;
    std::shared_ptr<VirtualCameraFrameSource> mFrameSource;
    std::shared_ptr<VirtualCameraFrameSourceV2> mFrameSourceV2;
    
    // Format: device@<major>.<minor>/<type>/<id>
    // Note: ID must be unique across all camera providers (0-2 used by internal/0)
    static constexpr const char* kVirtualCameraId = "device@1.0/virtual_renderer/100";
};

}  // namespace
