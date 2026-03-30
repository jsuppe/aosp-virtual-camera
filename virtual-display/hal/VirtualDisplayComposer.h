/*
 * VirtualDisplayComposer - IComposer implementation for virtual display
 * 
 * Presents a virtual display as the primary display to SurfaceFlinger.
 * Frames are output to a BufferQueue for consumption by a renderer app.
 */
#pragma once

#include <aidl/android/hardware/graphics/composer3/BnComposer.h>
#include <aidl/android/hardware/graphics/composer3/IComposerClient.h>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace aidl::android::hardware::graphics::composer3 {

class VirtualDisplayClient;

class VirtualDisplayComposer : public BnComposer {
public:
    VirtualDisplayComposer();
    ~VirtualDisplayComposer() override = default;

    // IComposer interface
    ndk::ScopedAStatus createClient(std::shared_ptr<IComposerClient>* outClient) override;
    ndk::ScopedAStatus getCapabilities(std::vector<Capability>* outCapabilities) override;

    // Configuration (set before SurfaceFlinger connects)
    void setVirtualDisplayConfig(int32_t width, int32_t height, 
                                  float refreshRate, int32_t densityDpi);

    // Check if renderer app is connected
    bool isRendererConnected() const;

private:
    std::mutex mLock;
    std::weak_ptr<VirtualDisplayClient> mClient;
    
    // Virtual display configuration (renderer-defined)
    int32_t mWidth = 1920;
    int32_t mHeight = 1080;
    float mRefreshRate = 60.0f;
    int32_t mDensityDpi = 420;
    
    bool mRendererConnected = false;
};

}  // namespace aidl::android::hardware::graphics::composer3
