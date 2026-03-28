/*
 * VirtualCameraDevice - Implementation
 * 
 * Uses IVirtualCameraManager AIDL for all buffer operations.
 * 
 * Location: hardware/interfaces/camera/provider/virtual/
 */
#define LOG_TAG "VirtualCameraDevice"

#include "VirtualCameraDevice.h"
#include "VirtualCameraProvider.h"
#include <android-base/logging.h>
#include <system/camera_metadata.h>
#include <aidlcommonsupport/NativeHandle.h>

namespace aidl::android::hardware::camera::provider::implementation {

using namespace ::aidl::android::hardware::camera::device;

// ============ VirtualCameraDevice ============

VirtualCameraDevice::VirtualCameraDevice(int cameraId,
                                         const VirtualCameraConfig& config,
                                         VirtualCameraProvider* provider)
    : mCameraId(cameraId), mConfig(config), mProvider(provider) {
    buildCharacteristics();
}

VirtualCameraDevice::~VirtualCameraDevice() = default;

void VirtualCameraDevice::buildCharacteristics() {
    camera_metadata_t* meta = allocate_camera_metadata(50, 500);
    
    // Facing
    uint8_t facing = mConfig.facing;
    add_camera_metadata_entry(meta, ANDROID_LENS_FACING, &facing, 1);
    
    // Orientation
    int32_t orientation = mConfig.orientation;
    add_camera_metadata_entry(meta, ANDROID_SENSOR_ORIENTATION, &orientation, 1);
    
    // Stream configurations
    std::vector<int32_t> streamConfigs;
    for (int32_t format : mConfig.supportedFormats) {
        streamConfigs.push_back(format);
        streamConfigs.push_back(mConfig.maxWidth);
        streamConfigs.push_back(mConfig.maxHeight);
        streamConfigs.push_back(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
        
        // Add common resolutions
        int resolutions[][2] = {{1920, 1080}, {1280, 720}, {640, 480}};
        for (auto& res : resolutions) {
            if (res[0] <= mConfig.maxWidth && res[1] <= mConfig.maxHeight) {
                streamConfigs.push_back(format);
                streamConfigs.push_back(res[0]);
                streamConfigs.push_back(res[1]);
                streamConfigs.push_back(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
            }
        }
    }
    add_camera_metadata_entry(meta, 
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
        streamConfigs.data(), streamConfigs.size());
    
    // Frame durations
    int64_t minFrameDuration = 1000000000LL / mConfig.maxFps;
    std::vector<int64_t> frameDurations;
    for (int32_t format : mConfig.supportedFormats) {
        frameDurations.push_back(format);
        frameDurations.push_back(mConfig.maxWidth);
        frameDurations.push_back(mConfig.maxHeight);
        frameDurations.push_back(minFrameDuration);
    }
    add_camera_metadata_entry(meta,
        ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
        frameDurations.data(), frameDurations.size());
    
    // Hardware level
    uint8_t hwLevel = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL;
    add_camera_metadata_entry(meta, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &hwLevel, 1);
    
    // Active array
    int32_t activeArray[] = {0, 0, mConfig.maxWidth, mConfig.maxHeight};
    add_camera_metadata_entry(meta, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, activeArray, 4);
    
    // Pixel array
    int32_t pixelArray[] = {mConfig.maxWidth, mConfig.maxHeight};
    add_camera_metadata_entry(meta, ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, pixelArray, 2);
    
    // Capabilities
    uint8_t capabilities[] = {ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE};
    add_camera_metadata_entry(meta, ANDROID_REQUEST_AVAILABLE_CAPABILITIES, 
        capabilities, sizeof(capabilities));
    
    // Convert to AIDL format
    mCharacteristics.metadata.assign(
        reinterpret_cast<uint8_t*>(meta),
        reinterpret_cast<uint8_t*>(meta) + get_camera_metadata_size(meta));
    
    free_camera_metadata(meta);
}

ndk::ScopedAStatus VirtualCameraDevice::getCameraCharacteristics(
        CameraMetadata* characteristics) {
    *characteristics = mCharacteristics;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraDevice::getPhysicalCameraCharacteristics(
        const std::string&, CameraMetadata*) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(Status::ILLEGAL_ARGUMENT));
}

ndk::ScopedAStatus VirtualCameraDevice::getResourceCost(CameraResourceCost* cost) {
    cost->resourceCost = 50;
    cost->conflictingDevices.clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraDevice::isStreamCombinationSupported(
        const StreamConfiguration& streams, bool* supported) {
    *supported = true;
    
    for (const auto& stream : streams.streams) {
        bool formatSupported = false;
        for (int32_t fmt : mConfig.supportedFormats) {
            if (static_cast<int32_t>(stream.format) == fmt) {
                formatSupported = true;
                break;
            }
        }
        if (!formatSupported) {
            *supported = false;
            return ndk::ScopedAStatus::ok();
        }
        
        if (stream.width > mConfig.maxWidth || stream.height > mConfig.maxHeight) {
            *supported = false;
            return ndk::ScopedAStatus::ok();
        }
    }
    
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraDevice::open(
        const std::shared_ptr<ICameraDeviceCallback>& callback,
        std::shared_ptr<ICameraDeviceSession>* session) {
    
    LOG(INFO) << "Opening virtual camera: " << mCameraId;
    *session = ndk::SharedRefBase::make<VirtualCameraSession>(this, callback);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraDevice::openInjectionSession(
        const std::shared_ptr<ICameraDeviceCallback>&,
        std::shared_ptr<ICameraInjectionSession>*) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(Status::OPERATION_NOT_SUPPORTED));
}

ndk::ScopedAStatus VirtualCameraDevice::setTorchMode(bool) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(Status::OPERATION_NOT_SUPPORTED));
}

ndk::ScopedAStatus VirtualCameraDevice::turnOnTorchWithStrengthLevel(int32_t) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(Status::OPERATION_NOT_SUPPORTED));
}

ndk::ScopedAStatus VirtualCameraDevice::getTorchStrengthLevel(int32_t*) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(Status::OPERATION_NOT_SUPPORTED));
}

// ============ VirtualCameraSession ============

VirtualCameraSession::VirtualCameraSession(
        VirtualCameraDevice* device,
        std::shared_ptr<ICameraDeviceCallback> callback)
    : mDevice(device), mCallback(callback) {
    
    mManager = device->getProvider()->getManager();
    
    // Notify service that camera is opened
    if (mManager) {
        mManager->notifyCameraOpened(device->getCameraId());
    }
    
    // Start request thread
    mRequestThreadRunning = true;
    mRequestThread = std::thread(&VirtualCameraSession::requestThreadLoop, this);
}

VirtualCameraSession::~VirtualCameraSession() {
    close();
}

ndk::ScopedAStatus VirtualCameraSession::close() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mClosed) return ndk::ScopedAStatus::ok();
        mClosed = true;
        mRequestThreadRunning = false;
    }
    
    mRequestCv.notify_all();
    if (mRequestThread.joinable()) {
        mRequestThread.join();
    }
    
    // Notify service
    if (mManager) {
        mManager->notifyCameraClosed(mDevice->getCameraId());
    }
    
    LOG(INFO) << "Closed virtual camera session: " << mDevice->getCameraId();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::configureStreams(
        const StreamConfiguration& config,
        std::vector<HalStream>* halStreams) {
    
    std::lock_guard<std::mutex> lock(mMutex);
    
    mStreams.clear();
    halStreams->clear();
    
    // Convert to StreamConfig for service
    std::vector<StreamConfig> serviceStreams;
    
    for (const auto& stream : config.streams) {
        StreamConfig cfg;
        cfg.streamId = stream.id;
        cfg.width = stream.width;
        cfg.height = stream.height;
        cfg.format = static_cast<int32_t>(stream.format);
        cfg.fps = mDevice->getConfig().maxFps;
        cfg.usage = stream.usage;
        cfg.useCase = 0;
        
        mStreams.push_back(cfg);
        serviceStreams.push_back(cfg);
        
        HalStream halStream;
        halStream.id = stream.id;
        halStream.overrideFormat = stream.format;
        halStream.producerUsage = stream.usage;
        halStream.consumerUsage = 0;
        halStream.maxBuffers = 4;
        halStream.overrideDataSpace = stream.dataSpace;
        halStream.physicalCameraId = "";
        halStream.supportOffline = false;
        halStreams->push_back(halStream);
        
        LOG(INFO) << "Configured stream " << stream.id 
                  << ": " << stream.width << "x" << stream.height;
    }
    
    // Notify service of stream configuration
    if (mManager) {
        mManager->notifyStreamsConfigured(
            mDevice->getCameraId(), 
            serviceStreams);
    }
    
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::constructDefaultRequestSettings(
        RequestTemplate, CameraMetadata* settings) {
    
    camera_metadata_t* meta = allocate_camera_metadata(10, 100);
    uint8_t controlMode = ANDROID_CONTROL_MODE_AUTO;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_MODE, &controlMode, 1);
    
    settings->metadata.assign(
        reinterpret_cast<uint8_t*>(meta),
        reinterpret_cast<uint8_t*>(meta) + get_camera_metadata_size(meta));
    free_camera_metadata(meta);
    
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::processCaptureRequest(
        const std::vector<CaptureRequest>& requests,
        const std::vector<BufferCache>&,
        int32_t* numRequestProcessed) {
    
    std::lock_guard<std::mutex> lock(mMutex);
    
    if (mClosed) {
        return ndk::ScopedAStatus::fromServiceSpecificError(
            static_cast<int32_t>(Status::INTERNAL_ERROR));
    }
    
    // Notify service that capture is starting (on first request)
    static bool captureStarted = false;
    if (!captureStarted && mManager) {
        mManager->notifyCaptureStarted(
            mDevice->getCameraId(), 
            mDevice->getConfig().maxFps);
        captureStarted = true;
    }
    
    for (const auto& request : requests) {
        mPendingRequests.push(request);
    }
    
    *numRequestProcessed = requests.size();
    mRequestCv.notify_one();
    
    return ndk::ScopedAStatus::ok();
}

void VirtualCameraSession::requestThreadLoop() {
    while (mRequestThreadRunning) {
        CaptureRequest request;
        
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mRequestCv.wait(lock, [this] {
                return !mPendingRequests.empty() || !mRequestThreadRunning;
            });
            
            if (!mRequestThreadRunning) break;
            
            request = mPendingRequests.front();
            mPendingRequests.pop();
        }
        
        processRequest(request);
    }
}

void VirtualCameraSession::processRequest(const CaptureRequest& request) {
    int cameraId = mDevice->getCameraId();
    std::vector<StreamBuffer> outputBuffers;
    
    for (const auto& buffer : request.outputBuffers) {
        StreamBuffer outBuffer = buffer;
        
        // Acquire buffer from renderer via service
        if (mManager) {
            ::aidl::android::hardware::HardwareBuffer hwBuffer;
            auto status = mManager->acquireBuffer(cameraId, buffer.streamId, &hwBuffer);
            
            if (status.isOk() && hwBuffer.handle.fds.size() > 0) {
                // Got a buffer from renderer!
                // The HardwareBuffer contains the rendered frame
                // We need to copy/map it to the output buffer handle
                // 
                // In a real implementation, you'd use GraphicBufferMapper
                // to map both buffers and copy, or ideally share the same
                // underlying buffer.
                
                outBuffer.status = BufferStatus::OK;
                
                // Release buffer back to renderer
                mManager->releaseBuffer(cameraId, buffer.streamId, hwBuffer);
            } else {
                outBuffer.status = BufferStatus::ERROR;
            }
        } else {
            outBuffer.status = BufferStatus::ERROR;
        }
        
        outputBuffers.push_back(outBuffer);
    }
    
    // Build result
    CaptureResult result;
    result.frameNumber = request.frameNumber;
    result.outputBuffers = outputBuffers;
    result.inputBuffer.streamId = -1;
    result.partialResult = 1;
    
    camera_metadata_t* meta = allocate_camera_metadata(5, 50);
    int64_t timestamp = systemTime(SYSTEM_TIME_MONOTONIC);
    add_camera_metadata_entry(meta, ANDROID_SENSOR_TIMESTAMP, &timestamp, 1);
    
    result.result.metadata.assign(
        reinterpret_cast<uint8_t*>(meta),
        reinterpret_cast<uint8_t*>(meta) + get_camera_metadata_size(meta));
    free_camera_metadata(meta);
    
    // Send to framework
    std::vector<CaptureResult> results{result};
    mCallback->processCaptureResult(results);
}

ndk::ScopedAStatus VirtualCameraSession::signalStreamFlush(
        const std::vector<int32_t>&, int32_t) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::flush() {
    std::lock_guard<std::mutex> lock(mMutex);
    while (!mPendingRequests.empty()) {
        mPendingRequests.pop();
    }
    
    if (mManager) {
        mManager->notifyCaptureStopped(mDevice->getCameraId());
    }
    
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::switchToOffline(
        const std::vector<int32_t>&,
        CameraOfflineSessionInfo*,
        std::shared_ptr<ICameraOfflineSession>*) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(Status::OPERATION_NOT_SUPPORTED));
}

ndk::ScopedAStatus VirtualCameraSession::repeatingRequestEnd(
        int32_t, const std::vector<int32_t>&) {
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::camera::provider::implementation
