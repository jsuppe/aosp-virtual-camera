/*
 * IVirtualCameraHalListener - Service to HAL availability push
 *
 * The camera HAL registers one of these with VirtualCameraService (via
 * IVirtualCameraManager.setHalListener). The service pushes producer
 * availability so the HAL can dynamically add/remove the virtual camera
 * device (ICameraProviderCallback.cameraDeviceStatusChange) — making
 * Camera2 availability events track "a producer is registered".
 * @hide
 */
package android.hardware.virtualcamera;

/**
 * Callback from VirtualCameraService into the camera HAL.
 * @hide
 */
interface IVirtualCameraHalListener {
    /**
     * Fired when producer availability transitions (0 -> >0 registered
     * producers, or back to 0). Also fired once with the current state
     * immediately after the listener is registered.
     */
    void onProducerAvailabilityChanged(boolean available);
}
