/*
 * IVirtualMicService - Service interface for renderer apps
 * 
 * Allows apps to register as virtual microphone renderers,
 * providing audio that appears as mic input to other apps.
 */
package com.android.virtualmicservice;

import com.android.virtualmicservice.IVirtualMicSession;
import com.android.virtualmicservice.VirtualMicConfig;
import com.android.virtualmicservice.RendererInfo;

interface IVirtualMicService {
    /**
     * Check if a virtual mic session can be started.
     * Returns false if another renderer is already active.
     */
    boolean isAvailable();
    
    /**
     * Register a virtual microphone with specified characteristics.
     * Returns a session for providing audio data.
     * Returns null if registration fails (e.g., another renderer active).
     */
    IVirtualMicSession registerMic(in VirtualMicConfig config);
    
    /**
     * Get info about the currently active renderer, if any.
     */
    @nullable RendererInfo getCurrentRenderer();
    
    /**
     * List all registered virtual microphones.
     */
    List<RendererInfo> getRegisteredMics();
}
