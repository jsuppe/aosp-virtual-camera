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

    /** V3: per-slot forms. A V3 HAL calls these for every slot (including 0)
     *  when the platform callback is V3; it falls back to the unslotted
     *  methods for slot 0 against a V2 platform. */
    void onStreamsConfiguredForCamera(int slot, int width, int height, int fps);
    void onCameraClosedForCamera(int slot);
}
