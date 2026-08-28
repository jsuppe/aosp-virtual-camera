/*
 * IVirtualCameraManager - HAL to Service Interface
 *
 * A13-compatible design: the HAL owns the BufferQueues and passes the
 * producer-side Surfaces up to the service, which relays them to the
 * registered renderer app (IVirtualCameraCallback.onStreamsConfigured).
 * Frames then flow zero-copy through the BufferQueue without crossing
 * this interface.
 * @hide
 */
package android.hardware.virtualcamera;

import android.hardware.virtualcamera.IVirtualCameraHalListener;
import android.hardware.virtualcamera.VirtualCameraConfig;
import android.hardware.virtualcamera.StreamConfig;
import android.view.Surface;

/**
 * Interface for Camera HAL to interact with VirtualCameraService.
 * @hide
 */
interface IVirtualCameraManager {
    int[] getRegisteredCameraIds();
    VirtualCameraConfig getCameraConfig(int cameraId);
    void notifyCameraOpened(int cameraId);
    /** HAL created one BufferQueue per stream; surfaces[i] is the producer end for streams[i]. */
    void notifyStreamsConfigured(int cameraId, in StreamConfig[] streams, in Surface[] surfaces);
    void notifyCaptureStarted(int cameraId, int frameRate);
    void notifyCaptureStopped(int cameraId);
    void notifyCameraClosed(int cameraId);

    /**
     * Register the HAL's availability listener. The service immediately
     * pushes the current producer availability, then pushes every
     * 0<->N transition (register/unregister/producer death).
     */
    void setHalListener(IVirtualCameraHalListener listener);
}
