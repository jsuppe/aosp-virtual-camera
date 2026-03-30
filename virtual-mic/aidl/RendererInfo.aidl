/*
 * RendererInfo - Information about an active renderer
 */
package com.android.virtualmicservice;

parcelable RendererInfo {
    /**
     * UID of the renderer app.
     */
    int uid;
    
    /**
     * Package name of the renderer app.
     */
    String packageName;
    
    /**
     * Product name of the virtual mic.
     */
    String productName;
    
    /**
     * Device address/identifier.
     */
    String address;
    
    /**
     * Active sample rate.
     */
    int sampleRate;
    
    /**
     * Active channel count.
     */
    int channelCount;
    
    /**
     * Timestamp when session started (SystemClock.elapsedRealtime).
     */
    long startTimeMs;
    
    /**
     * Total frames provided so far.
     */
    long framesProvided;
    
    /**
     * Total underruns (consumer read when no data).
     */
    int underrunCount;
}
