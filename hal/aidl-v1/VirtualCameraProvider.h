/*
 * VirtualCameraProvider - AIDL V1 Camera Provider Adapter
 *
 * Thin adapter that implements the Camera AIDL V1 BnCameraProvider interface.
 * Delegates frame pipeline work to core/.
 */
#pragma once

#include <aidl/android/hardware/camera/provider/BnCameraProvider.h>
#include <aidl/android/hardware/camera/provider/ICameraProviderCallback.h>
#include <mutex>
#include <memory>

// Forward declarations (avoid pulling heavy core headers into service.cpp)
namespace virtualcamera {
class VirtualCameraFrameSource;
class VirtualCameraFrameSourceV2;
}

namespace aidl::android::hardware::camera::provider::implementation {

class VirtualCameraProvider : public BnCameraProvider {
public:
    VirtualCameraProvider();
    ~VirtualCameraProvider() override;

    // Access frame sources (used by VirtualCameraDevice to pass to sessions)
    std::shared_ptr<virtualcamera::VirtualCameraFrameSource> getFrameSource() const {
        return mFrameSource;
    }
    std::shared_ptr<virtualcamera::VirtualCameraFrameSourceV2> getFrameSourceV2() const {
        return mFrameSourceV2;
    }

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
    std::shared_ptr<virtualcamera::VirtualCameraFrameSource> mFrameSource;
    std::shared_ptr<virtualcamera::VirtualCameraFrameSourceV2> mFrameSourceV2;

    // Format: device@<major>.<minor>/<type>/<id>
    // Note: ID must be unique across all camera providers (0-2 used by internal/0)
    static constexpr const char* kVirtualCameraId = "device@1.0/virtual_renderer/100";
};

}  // namespace aidl::android::hardware::camera::provider::implementation
