/*
 * Virtual Camera Service - AIDL Interface
 * @hide
 */
package android.hardware.virtualcamera;

import android.hardware.virtualcamera.IVirtualCameraCallback;
import android.hardware.virtualcamera.VirtualCameraConfig;

/**
 * System service interface for virtual camera management.
 * Apps use this to register as renderers for virtual cameras.
 * @hide
 */
interface IVirtualCameraService {
    int registerCamera(in VirtualCameraConfig config, IVirtualCameraCallback callback);
    void unregisterCamera(int cameraId);
    int[] getRegisteredCameras();
    boolean isCameraInUse(int cameraId);
}
