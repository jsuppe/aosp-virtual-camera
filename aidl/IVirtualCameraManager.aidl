/*
 * IVirtualCameraManager - HAL to Service Interface
 * @hide
 */
package android.hardware.camera.virtual;

import android.hardware.camera.virtual.VirtualCameraConfig;
import android.hardware.camera.virtual.StreamConfig;
import android.hardware.HardwareBuffer;

/**
 * Interface for Camera HAL to interact with VirtualCameraService.
 * @hide
 */
interface IVirtualCameraManager {
    int[] getRegisteredCameraIds();
    VirtualCameraConfig getCameraConfig(int cameraId);
    void notifyCameraOpened(int cameraId);
    void notifyStreamsConfigured(int cameraId, in StreamConfig[] streams);
    void notifyCaptureStarted(int cameraId, int frameRate);
    void notifyCaptureStopped(int cameraId);
    void notifyCameraClosed(int cameraId);
    @nullable HardwareBuffer acquireBuffer(int cameraId, int streamId);
    void releaseBuffer(int cameraId, int streamId, in HardwareBuffer buffer);
    @nullable HardwareBuffer requestStillCapture(int cameraId, int captureId);
}
