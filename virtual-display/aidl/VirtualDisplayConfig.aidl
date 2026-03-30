/*
 * VirtualDisplayConfig - Configuration for virtual display registration
 * 
 * Unlike camera/mic where renderer defines what consumers receive,
 * display config defines what ALL APPS render to - system-wide impact!
 */
package com.android.virtualdisplayservice;

parcelable VirtualDisplayConfig {
    // --- Display Identity ---
    
    /**
     * Human-readable display name.
     */
    String name = "Virtual Display";
    
    // --- Resolution & Density ---
    
    /**
     * Display width in pixels.
     * All apps will render to this width.
     */
    int width = 1920;
    
    /**
     * Display height in pixels.
     * All apps will render to this height.
     */
    int height = 1080;
    
    /**
     * Display density in DPI.
     * Affects text size, drawable selection, layout calculations.
     */
    int densityDpi = 420;
    
    // --- Refresh Rate ---
    
    /**
     * Active refresh rate in Hz.
     */
    float refreshRate = 60.0f;
    
    /**
     * List of supported refresh rates.
     * System may switch between these based on content.
     */
    float[] supportedRefreshRates = {60.0f};
    
    // --- Color & HDR ---
    
    /**
     * Color mode (see ColorMode constants).
     * NATIVE=0, SRGB=1, DISPLAY_P3=2, etc.
     */
    int colorMode = 0;
    
    /**
     * HDR capabilities (bitmask).
     * HDR10=1, HLG=2, DOLBY_VISION=4, HDR10_PLUS=8
     */
    int hdrCapabilities = 0;
    
    /**
     * Peak HDR luminance in nits (if HDR supported).
     */
    float hdrMaxLuminance = 500.0f;
    
    // --- Additional Capabilities ---
    
    /**
     * Whether protected content (DRM) is supported.
     * Requires HDCP on physical display.
     */
    boolean supportsProtectedContent = false;
    
    /**
     * Supported color formats (bitmask).
     * RGBA_8888=1, RGBX_8888=2, RGB_888=4, etc.
     */
    int supportedFormats = 1;
    
    /**
     * Whether variable refresh rate (VRR/GSYNC/FreeSync) is supported.
     */
    boolean supportsVrr = false;
    
    // --- Orientation ---
    
    /**
     * Display orientation in degrees (0, 90, 180, 270).
     */
    int orientation = 0;
}
