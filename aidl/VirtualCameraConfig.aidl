/*
 * Virtual Camera Configuration
 * @hide
 */
package android.hardware.camera.virtual;

/**
 * Configuration for registering a virtual camera.
 * @hide
 */
parcelable VirtualCameraConfig {
    @nullable String name;
    int maxWidth = 3840;
    int maxHeight = 2160;
    int maxFps = 60;
    int[] supportedFormats;
    int facing = 2;
    int orientation = 0;
    boolean supportsJpeg = true;
    @nullable String uniqueId;
}
