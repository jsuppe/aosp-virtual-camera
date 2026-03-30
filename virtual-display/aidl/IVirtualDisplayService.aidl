/*
 * IVirtualDisplayService - Service interface for display renderer apps
 * 
 * Allows apps to register as the display renderer, receiving composited
 * frames from all apps and outputting to the physical display.
 */
package com.android.virtualdisplayservice;

import com.android.virtualdisplayservice.IVirtualDisplaySession;
import com.android.virtualdisplayservice.VirtualDisplayConfig;
import com.android.virtualdisplayservice.RendererInfo;

interface IVirtualDisplayService {
    /**
     * Check if display renderer registration is available.
     * Returns false if another renderer is already active.
     */
    boolean isAvailable();
    
    /**
     * Register as the display renderer with specified characteristics.
     * This triggers a system-wide display configuration change.
     * All apps will receive onConfigurationChanged().
     * 
     * @param config Display configuration (resolution, refresh rate, etc.)
     * @return Session for receiving frames, or null if registration fails
     */
    IVirtualDisplaySession registerDisplay(in VirtualDisplayConfig config);
    
    /**
     * Get info about the currently active renderer, if any.
     */
    @nullable RendererInfo getCurrentRenderer();
    
    /**
     * Get the physical display info (for renderer to know output target).
     */
    PhysicalDisplayInfo getPhysicalDisplayInfo();
}
