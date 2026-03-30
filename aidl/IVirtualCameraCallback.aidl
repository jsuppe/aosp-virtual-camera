/*
 * Virtual Camera Callback - AIDL Interface
 * @hide
 */
package android.hardware.camera.virtual;

import android.view.Surface;
import android.hardware.camera.virtual.StreamConfig;

/**
 * Callback interface for renderer apps.
 * @hide
 */
interface IVirtualCameraCallback {
    void onCameraOpened();
    void onStreamsConfigured(in StreamConfig[] streams, in Surface[] surfaces);
    void onCaptureStarted(int frameRate);
    void onCaptureStopped();
    void onCameraClosed();
    void onStillCaptureRequested(in Surface surface, int captureId);
}
