/*
 * AidlFrameSource - Platform-AIDL frame source implementation (A13)
 */

#ifdef VCAM_AIDL_SOURCE

#define LOG_TAG "VCamAidlSource"

#include "AidlFrameSource.h"

#include <android/hardware/virtualcamera/IVirtualCameraManager.h>
#include <android/hardware/virtualcamera/StreamConfig.h>
#include <binder/IServiceManager.h>
#include <gui/BufferQueue.h>
#include <gui/view/Surface.h>
#include <hardware/gralloc.h>
#include <log/log.h>
#include <system/graphics.h>
#include <ui/GraphicBuffer.h>

#include <algorithm>

namespace virtualcamera {

namespace vcaidl = ::android::hardware::virtualcamera;
using ::android::BufferItem;
using ::android::BufferItemConsumer;
using ::android::BufferQueue;
using ::android::GraphicBuffer;
using ::android::IGraphicBufferConsumer;
using ::android::IGraphicBufferProducer;
using ::android::sp;
using ::android::String16;
using ::android::String8;

AidlFrameSource::~AidlFrameSource() {
    std::lock_guard<std::mutex> lock(mLock);
    teardownLocked();
}

sp<vcaidl::IVirtualCameraManager> AidlFrameSource::getManager() {
    if (mManager != nullptr) {
        return mManager;
    }
    sp<::android::IBinder> binder =
            ::android::defaultServiceManager()->checkService(
                    String16("virtual_camera_manager"));
    if (binder == nullptr) {
        return nullptr;
    }
    mManager = ::android::interface_cast<vcaidl::IVirtualCameraManager>(binder);
    if (mManager != nullptr) {
        ALOGI("Connected to virtual_camera_manager");
    }
    return mManager;
}

bool AidlFrameSource::configureStreams(int width, int height, int fps) {
    std::lock_guard<std::mutex> lock(mLock);
    mWidth = width;
    mHeight = height;
    mFps = fps > 0 ? fps : 30;
    teardownLocked();
    return doConfigureLocked();
}

void AidlFrameSource::retryIfNeeded() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mConfigured || mWidth <= 0) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - mLastRetry < std::chrono::seconds(1)) {
        return;
    }
    mLastRetry = now;
    doConfigureLocked();
}

bool AidlFrameSource::doConfigureLocked() {
    auto mgr = getManager();
    if (mgr == nullptr) {
        return false;
    }

    std::vector<int32_t> ids;
    ::android::binder::Status status = mgr->getRegisteredCameraIds(&ids);
    if (!status.isOk()) {
        ALOGW("getRegisteredCameraIds failed: %s", status.toString8().c_str());
        mManager = nullptr;  // stale binder, re-resolve next time
        return false;
    }
    if (ids.empty()) {
        return false;  // no producer registered yet
    }
    mCameraId = ids[0];

    // Create the BufferQueue this stream will be fed through
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);

    // SW_READ so the HAL can read frames on the CPU; SW_WRITE so the producer
    // app can render into the Surface with software Canvas (lockCanvas).
    mConsumer = new BufferItemConsumer(consumer,
            GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN,
            /*maxAcquiredBuffers*/ 2,
            /*controlledByApp*/ false);
    mConsumer->setName(String8("VirtualCameraAidlStream"));
    mConsumer->setDefaultBufferSize(mWidth, mHeight);
    mConsumer->setDefaultBufferFormat(HAL_PIXEL_FORMAT_RGBA_8888);

    ::android::view::Surface viewSurface;
    viewSurface.name = String16("VirtualCameraAidlStream");
    viewSurface.graphicBufferProducer = producer;

    vcaidl::StreamConfig sc;
    sc.streamId = 0;
    sc.width = mWidth;
    sc.height = mHeight;
    sc.format = HAL_PIXEL_FORMAT_RGBA_8888;
    sc.fps = mFps;
    sc.usage = 0;
    sc.useCase = 0;

    std::vector<vcaidl::StreamConfig> streams{sc};
    std::vector<::android::view::Surface> surfaces{viewSurface};

    mgr->notifyCameraOpened(mCameraId);
    status = mgr->notifyStreamsConfigured(mCameraId, streams, surfaces);
    if (!status.isOk()) {
        ALOGE("notifyStreamsConfigured failed: %s", status.toString8().c_str());
        teardownLocked();
        return false;
    }
    mgr->notifyCaptureStarted(mCameraId, mFps);

    mConfigured = true;
    mFramesFilled = 0;
    ALOGI("AIDL stream configured: camera %d, %dx%d@%d, Surface delivered to producer",
          mCameraId, mWidth, mHeight, mFps);
    return true;
}

void AidlFrameSource::teardownLocked() {
    if (mHasHeld && mConsumer != nullptr) {
        mConsumer->releaseBuffer(mHeld);
    }
    mHasHeld = false;
    if (mConsumer != nullptr) {
        mConsumer->abandon();
        mConsumer.clear();
    }
    mConfigured = false;
}

void AidlFrameSource::sessionClosed() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mConfigured && mManager != nullptr && mCameraId >= 0) {
        mManager->notifyCaptureStopped(mCameraId);
        mManager->notifyCameraClosed(mCameraId);
    }
    teardownLocked();
}

bool AidlFrameSource::isActive() {
    std::lock_guard<std::mutex> lock(mLock);
    return mConfigured;
}

bool AidlFrameSource::fillLatestInto(HandleImporter& importer,
                                     buffer_handle_t handle,
                                     int width, int height) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mConfigured || mConsumer == nullptr || handle == nullptr) {
        return false;
    }

    // Drain the queue, keeping only the newest item
    BufferItem item;
    while (mConsumer->acquireBuffer(&item, 0, /*waitForFence*/ false) ==
           ::android::NO_ERROR) {
        if (mHasHeld) {
            mConsumer->releaseBuffer(mHeld);
        }
        mHeld = item;
        mHasHeld = true;
    }

    if (!mHasHeld) {
        return false;  // producer has not drawn a frame yet
    }

    sp<GraphicBuffer> gb = mHeld.mGraphicBuffer;
    if (gb == nullptr) {
        return false;
    }

    if (mHeld.mFence != nullptr && mHeld.mFence->isValid()) {
        mHeld.mFence->waitForever("VCamAidlSource::fill");
    }

    void* srcPtr = nullptr;
    if (gb->lock(GRALLOC_USAGE_SW_READ_OFTEN, &srcPtr) != ::android::NO_ERROR ||
        srcPtr == nullptr) {
        ALOGE("Failed to lock producer GraphicBuffer");
        return false;
    }

    auto ycbcr = lockYCbCrCompat(importer, handle,
            0x00000030U /*GRALLOC_USAGE_SW_WRITE_OFTEN*/, width, height);
    if (ycbcr.y == nullptr) {
        ALOGE("Failed to lock output buffer");
        gb->unlock();
        return false;
    }

    const uint8_t* rgba = static_cast<const uint8_t*>(srcPtr);
    const uint32_t srcStride = gb->getStride() * 4;

    uint8_t* yPlane = static_cast<uint8_t*>(ycbcr.y);
    uint8_t* cbPlane = static_cast<uint8_t*>(ycbcr.cb);
    uint8_t* crPlane = static_cast<uint8_t*>(ycbcr.cr);
    const int yStride = ycbcr.ystride;
    const int cStride = ycbcr.cstride;
    const int chromaStep = ycbcr.chroma_step;

    const int copyWidth = std::min(width, static_cast<int>(gb->getWidth()));
    const int copyHeight = std::min(height, static_cast<int>(gb->getHeight()));

    for (int y = 0; y < copyHeight; y++) {
        const uint8_t* row = rgba + y * srcStride;
        uint8_t* yRow = yPlane + y * yStride;
        for (int x = 0; x < copyWidth; x++) {
            const uint8_t r = row[x * 4 + 0];
            const uint8_t g = row[x * 4 + 1];
            const uint8_t b = row[x * 4 + 2];
            yRow[x] = static_cast<uint8_t>(
                std::clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 0, 255));
        }
    }

    const int chromaH = copyHeight / 2;
    const int chromaW = copyWidth / 2;
    for (int cy = 0; cy < chromaH; cy++) {
        const uint8_t* row = rgba + (cy * 2) * srcStride;
        for (int cx = 0; cx < chromaW; cx++) {
            const uint8_t r = row[cx * 2 * 4 + 0];
            const uint8_t g = row[cx * 2 * 4 + 1];
            const uint8_t b = row[cx * 2 * 4 + 2];
            const uint8_t cbVal = static_cast<uint8_t>(
                std::clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 0, 255));
            const uint8_t crVal = static_cast<uint8_t>(
                std::clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 0, 255));
            if (chromaStep == 2) {
                cbPlane[cy * cStride + cx * 2] = cbVal;
                crPlane[cy * cStride + cx * 2] = crVal;
            } else {
                cbPlane[cy * cStride + cx] = cbVal;
                crPlane[cy * cStride + cx] = crVal;
            }
        }
    }

    importer.unlock(handle);
    gb->unlock();

    if ((++mFramesFilled % 100) == 1) {
        ALOGI("Filled %llu frames from AIDL producer (camera %d)",
              (unsigned long long)mFramesFilled, mCameraId);
    }
    return true;
}

}  // namespace virtualcamera

#endif  // VCAM_AIDL_SOURCE
