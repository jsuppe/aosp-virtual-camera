/*
 * Callback from the vendor virtual-camera HAL up to the platform
 * VirtualCameraService. Registered via IVirtualCameraHal.setCallback().
 */
package android.hardware.virtualcamera.hal;

@VintfStability
interface IVirtualCameraHalCallback {
    /**
     * A Camera2 consumer configured streams on the virtual camera. The
     * platform side should create the frame source (BufferQueue) of this
     * shape, hand its Surface to the registered producer app, and begin
     * pushing frames via IVirtualCameraHal.queueFrame().
     */
    void onStreamsConfigured(int width, int height, int fps);

    /** The Camera2 session closed; the platform side should tear down. */
    void onCameraClosed();
}
