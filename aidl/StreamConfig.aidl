/*
 * Stream Configuration
 * Location: frameworks/base/core/java/android/hardware/camera/virtual/
 */
package android.hardware.camera.virtual;

/**
 * Configuration for a single camera stream.
 * Sent to renderer when camera client configures capture session.
 */
parcelable StreamConfig {
    /** Stream ID */
    int streamId;
    
    /** Width in pixels */
    int width;
    
    /** Height in pixels */
    int height;
    
    /** Pixel format (HAL_PIXEL_FORMAT_*) */
    int format;
    
    /** Target frame rate */
    int fps;
    
    /** Usage flags (GRALLOC_USAGE_*) */
    long usage;
    
    /** Stream use case (preview, video, still, etc.) */
    int useCase;
}
