/*
 * Virtual Camera Service - AIDL Interface
 * 
 * Allows apps to register as frame renderers for virtual cameras.
 * Location: frameworks/base/core/java/android/hardware/camera/virtual/
 */
package android.hardware.camera.virtual;

import android.hardware.camera.virtual.IVirtualCameraCallback;
import android.hardware.camera.virtual.VirtualCameraConfig;
import android.view.Surface;

/**
 * System service interface for virtual camera management.
 * Apps use this to register as renderers for virtual cameras.
 */
interface IVirtualCameraService {
    
    /**
     * Register a new virtual camera.
     * 
     * @param config Camera configuration (resolution, format, fps, name)
     * @param callback Callback for camera lifecycle events
     * @return Unique camera ID, or -1 on failure
     */
    int registerCamera(in VirtualCameraConfig config, IVirtualCameraCallback callback);
    
    /**
     * Unregister a virtual camera.
     * 
     * @param cameraId The camera ID returned from registerCamera()
     */
    void unregisterCamera(int cameraId);
    
    /**
     * Get list of registered virtual camera IDs.
     */
    int[] getRegisteredCameras();
    
    /**
     * Check if a camera is currently being used by a client.
     */
    boolean isCameraInUse(int cameraId);
}
