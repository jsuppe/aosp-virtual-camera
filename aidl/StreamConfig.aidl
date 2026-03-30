/*
 * Stream Configuration
 * @hide
 */
package android.hardware.camera.virtual;

/**
 * Configuration for a single camera stream.
 * @hide
 */
parcelable StreamConfig {
    int streamId;
    int width;
    int height;
    int format;
    int fps;
    long usage;
    int useCase;
}
