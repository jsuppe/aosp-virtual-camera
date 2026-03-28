/*
 * IVirtualCameraManager - HAL ↔ Service Interface
 * 
 * Used by Camera HAL to communicate with VirtualCameraService.
 * Location: hardware/interfaces/camera/virtual/
 */
package android.hardware.camera.virtual;

import android.hardware.camera.virtual.VirtualCameraConfig;
import android.hardware.camera.virtual.StreamConfig;
import android.hardware.HardwareBuffer;

/**
 * Interface for Camera HAL to interact with VirtualCameraService.
 * HAL is the client, Service is the server.
 */
interface IVirtualCameraManager {
    
    /**
     * Get list of currently registered virtual camera IDs.
     */
    int[] getRegisteredCameraIds();
    
    /**
     * Get configuration for a specific camera.
     */
    VirtualCameraConfig getCameraConfig(int cameraId);
    
    /**
     * Notify service that a camera was opened by a client app.
     */
    void notifyCameraOpened(int cameraId);
    
    /**
     * Notify service that streams are being configured.
     * Service will create surfaces and notify renderer.
     * 
     * @param cameraId Camera being configured
     * @param streams Stream configurations requested by camera client
     */
    void notifyStreamsConfigured(int cameraId, in StreamConfig[] streams);
    
    /**
     * Notify service that capture is starting.
     * 
     * @param cameraId Camera starting capture
     * @param frameRate Target frame rate
     */
    void notifyCaptureStarted(int cameraId, int frameRate);
    
    /**
     * Notify service that capture has stopped.
     */
    void notifyCaptureStopped(int cameraId);
    
    /**
     * Notify service that camera was closed.
     */
    void notifyCameraClosed(int cameraId);
    
    /**
     * Acquire a buffer from the renderer for a stream.
     * Returns null if no buffer is available.
     * 
     * @param cameraId Camera requesting buffer
     * @param streamId Stream requesting buffer
     * @return HardwareBuffer containing rendered frame, or null
     */
    @nullable HardwareBuffer acquireBuffer(int cameraId, int streamId);
    
    /**
     * Release a buffer back to the renderer.
     * 
     * @param cameraId Camera releasing buffer
     * @param streamId Stream releasing buffer
     * @param buffer The buffer to release
     */
    void releaseBuffer(int cameraId, int streamId, in HardwareBuffer buffer);
    
    /**
     * Request a still capture frame from renderer.
     * 
     * @param cameraId Camera requesting capture
     * @param captureId Unique ID for this capture
     * @return HardwareBuffer containing high-quality frame
     */
    @nullable HardwareBuffer requestStillCapture(int cameraId, int captureId);
}
