/*
 * IVirtualDisplaySession - Session interface for active display renderer
 */
package com.android.virtualdisplayservice;

import android.view.Surface;
import android.os.ParcelFileDescriptor;

interface IVirtualDisplaySession {
    /**
     * Set callback for receiving composited frames.
     * Called when SurfaceFlinger presents a new frame.
     */
    void setFrameCallback(IFrameCallback callback);
    
    /**
     * Get Surface for receiving composited frames via BufferQueue.
     * Alternative to callback-based approach for GPU-based renderers.
     */
    Surface getInputSurface();
    
    /**
     * Get Surface for rendering to the physical display.
     * Renderer outputs to this surface after processing.
     */
    Surface getOutputSurface();
    
    /**
     * Get the current virtual display configuration.
     */
    VirtualDisplayConfig getConfig();
    
    /**
     * Update virtual display configuration (triggers system reconfiguration).
     * Use sparingly - causes all apps to receive onConfigurationChanged().
     */
    void updateConfig(in VirtualDisplayConfig newConfig);
    
    /**
     * Get frame statistics (dropped frames, latency, etc.).
     */
    FrameStats getFrameStats();
    
    /**
     * Signal that renderer has finished with a frame.
     * Required when using callback-based approach.
     */
    void releaseFrame(long frameId);
    
    /**
     * Unregister as display renderer.
     * System transitions back to passthrough mode.
     */
    void unregister();
}

/**
 * Callback for frame delivery.
 */
interface IFrameCallback {
    /**
     * Called when a new composited frame is available.
     * 
     * @param frameId Unique frame identifier (use with releaseFrame)
     * @param buffer Hardware buffer containing the frame
     * @param fence Sync fence to wait on before reading
     * @param timestampNs Presentation timestamp
     */
    void onFrame(long frameId, in HardwareBuffer buffer, 
                 in ParcelFileDescriptor fence, long timestampNs);
    
    /**
     * Called when display configuration changes.
     */
    void onConfigChanged(in VirtualDisplayConfig newConfig);
}
