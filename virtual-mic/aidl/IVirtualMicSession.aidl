/*
 * IVirtualMicSession - Session interface for active renderer
 */
package com.android.virtualmicservice;

import android.os.ParcelFileDescriptor;

interface IVirtualMicSession {
    /**
     * Write PCM audio samples to the virtual microphone.
     * Buffer format must match the config specified at registration.
     * 
     * @param buffer PCM samples (16-bit signed, interleaved if stereo)
     * @param offsetBytes Starting offset in buffer
     * @param sizeBytes Number of bytes to write
     * @return Number of bytes actually written
     */
    int write(in byte[] buffer, int offsetBytes, int sizeBytes);
    
    /**
     * Get a shared memory file descriptor for zero-copy writes.
     * More efficient than write() for high-throughput scenarios.
     * 
     * @param sizeBytes Requested buffer size
     * @return File descriptor for shared memory region
     */
    ParcelFileDescriptor getSharedBuffer(int sizeBytes);
    
    /**
     * Commit samples written to shared buffer.
     * 
     * @param sizeBytes Number of bytes written to shared buffer
     */
    void commitSharedBuffer(int sizeBytes);
    
    /**
     * Get current buffer state.
     * @return Number of frames currently buffered
     */
    int getBufferedFrameCount();
    
    /**
     * Get underrun count (consumer read when no data available).
     */
    int getUnderrunCount();
    
    /**
     * Get the active sample rate being used.
     */
    int getSampleRate();
    
    /**
     * Get the active channel count.
     */
    int getChannelCount();
    
    /**
     * Unregister this virtual microphone.
     * The mic will disappear from the device list.
     */
    void unregister();
}
