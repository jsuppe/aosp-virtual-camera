/*
 * AidlFrameSource - Platform-AIDL frame source (A13 relay design)
 *
 * The HAL owns a BufferQueue per configured stream. The producer-side Surface
 * (view::Surface / IGraphicBufferProducer) is handed to VirtualCameraService
 * (system_server) via IVirtualCameraManager.notifyStreamsConfigured(), which
 * relays it to the registered renderer app. The renderer draws into the
 * Surface; this class consumes BufferItems and converts RGBA -> YUV into the
 * camera output gralloc buffer.
 *
 * Only built when VCAM_AIDL_SOURCE is defined (system_ext build, A13).
 */
#pragma once

#ifdef VCAM_AIDL_SOURCE

#include <chrono>
#include <mutex>
#include <vector>

#include <gui/BufferItem.h>
#include <gui/BufferItemConsumer.h>
#include <utils/StrongPointer.h>

#include "HandleImporterCompat.h"

namespace android {
namespace hardware {
namespace virtualcamera {
class IVirtualCameraManager;
}  // namespace virtualcamera
}  // namespace hardware
}  // namespace android

namespace virtualcamera {

class AidlFrameSource {
public:
    AidlFrameSource() = default;
    ~AidlFrameSource();

    /**
     * Called from session configureStreams. Stores the stream shape and, if a
     * producer is registered with VirtualCameraService, creates the
     * BufferQueue and delivers the Surface. Safe to call when system_server
     * or the producer is not ready yet - retryIfNeeded() picks it up later.
     */
    bool configureStreams(int width, int height, int fps);

    /** Rate-limited (1s) re-attempt of configureStreams if not yet active. */
    void retryIfNeeded();

    /** Session closed: notify service, tear down the BufferQueue. */
    void sessionClosed();

    /** True when a producer is registered and its Surface was delivered. */
    bool isActive();

    /**
     * Copy the newest producer frame (RGBA) into the output gralloc buffer
     * (YUV). Returns false if no producer frame has arrived yet.
     */
    bool fillLatestInto(HandleImporter& importer, buffer_handle_t handle,
                        int width, int height);

private:
    ::android::sp<::android::hardware::virtualcamera::IVirtualCameraManager> getManager();
    bool doConfigureLocked();
    void teardownLocked();

    std::mutex mLock;
    ::android::sp<::android::hardware::virtualcamera::IVirtualCameraManager> mManager;

    // Requested stream shape (from configureStreams)
    int mWidth = 0;
    int mHeight = 0;
    int mFps = 30;

    // Active state
    bool mConfigured = false;
    int32_t mCameraId = -1;
    ::android::sp<::android::BufferItemConsumer> mConsumer;
    ::android::BufferItem mHeld;
    bool mHasHeld = false;
    uint64_t mFramesFilled = 0;

    std::chrono::steady_clock::time_point mLastRetry{};
};

}  // namespace virtualcamera

#endif  // VCAM_AIDL_SOURCE
