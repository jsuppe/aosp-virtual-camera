/*
 * Virtual Camera Configuration
 * Location: frameworks/base/core/java/android/hardware/camera/virtual/
 */
package android.hardware.camera.virtual;

/**
 * Configuration for registering a virtual camera.
 */
parcelable VirtualCameraConfig {
    /** Display name for the camera (shown in camera apps) */
    String name;
    
    /** Maximum supported width in pixels */
    int maxWidth = 3840;
    
    /** Maximum supported height in pixels */
    int maxHeight = 2160;
    
    /** Maximum supported frame rate */
    int maxFps = 60;
    
    /** 
     * Supported pixel formats (HAL_PIXEL_FORMAT_*)
     * Common values:
     *   1  = RGBA_8888 (4:4:4, 32bpp)
     *   2  = RGBX_8888
     *   3  = RGB_888 (24bpp)
     *  35  = YCbCr_420_888 (4:2:0)
     *  53  = YCbCr_444_888 (4:4:4) - if supported
     */
    int[] supportedFormats;
    
    /** Camera facing: 0 = back, 1 = front, 2 = external */
    int facing = 2;
    
    /** Sensor orientation in degrees (0, 90, 180, 270) */
    int orientation = 0;
    
    /** Whether to support JPEG capture */
    boolean supportsJpeg = true;
    
    /** Unique identifier (optional, auto-generated if empty) */
    String uniqueId;
}
