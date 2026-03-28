/*
 * Virtual Camera Callback - AIDL Interface
 * 
 * Callbacks from system to renderer app.
 * Location: frameworks/base/core/java/android/hardware/camera/virtual/
 */
package android.hardware.camera.virtual;

import android.view.Surface;
import android.hardware.camera.virtual.StreamConfig;

/**
 * Callback interface for renderer apps.
 * Called when camera clients open/configure/close the virtual camera.
 */
interface IVirtualCameraCallback {
    
    /**
     * Called when a camera client opens the virtual camera.
     * Renderer should prepare to start rendering.
     */
    void onCameraOpened();
    
    /**
     * Called when camera client configures capture session.
     * Renderer receives surfaces to render frames to.
     * 
     * @param streams Array of stream configurations
     * @param surfaces Array of surfaces to render to (one per stream)
     */
    void onStreamsConfigured(in StreamConfig[] streams, in Surface[] surfaces);
    
    /**
     * Called when capture session starts.
     * Renderer should begin producing frames.
     * 
     * @param frameRate Target frame rate in fps
     */
    void onCaptureStarted(int frameRate);
    
    /**
     * Called when capture session stops.
     * Renderer can pause frame production.
     */
    void onCaptureStopped();
    
    /**
     * Called when camera client closes the camera.
     * Renderer should release surfaces.
     */
    void onCameraClosed();
    
    /**
     * Called when a still capture is requested.
     * Renderer should produce a high-quality frame.
     * 
     * @param surface Surface for the still capture
     * @param captureId Unique ID for this capture request
     */
    void onStillCaptureRequested(in Surface surface, int captureId);
}
