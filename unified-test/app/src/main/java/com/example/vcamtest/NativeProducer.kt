package com.example.vcamtest

import android.os.ParcelFileDescriptor

/**
 * JNI bridge to native audio/video producers.
 * 
 * Now uses file descriptors from VirtualMediaService instead of
 * connecting directly to HAL sockets.
 */
object NativeProducer {
    
    init {
        System.loadLibrary("vmproducer")
    }
    
    // ========== Audio ==========
    
    /**
     * Start audio generation with a buffer fd from the service.
     * @param fd File descriptor for shared memory buffer
     * @param bufferSize Total size of the shared memory region
     * @param headerSize Size of the header (data starts at this offset)
     * @param sampleRate Audio sample rate
     * @param channels Number of audio channels
     * @param frequency Initial frequency in Hz
     * @param amplitude Initial amplitude (0.0 - 1.0)
     */
    external fun startAudioWithFd(
        fd: Int,
        bufferSize: Int,
        headerSize: Int,
        sampleRate: Int,
        channels: Int,
        frequency: Float,
        amplitude: Float
    ): Boolean
    
    external fun stopAudio()
    external fun isAudioRunning(): Boolean
    external fun setAudioFrequency(frequency: Float)
    external fun setAudioAmplitude(amplitude: Float)
    
    // ========== Video ==========
    
    /**
     * Start video generation with a buffer fd from the service.
     * @param fd File descriptor for shared memory buffer
     * @param bufferSize Total size of the shared memory region
     * @param headerSize Size of the header (data starts at this offset)
     * @param width Frame width in pixels
     * @param height Frame height in pixels
     * @param fps Frames per second
     */
    external fun startVideoWithFd(
        fd: Int,
        bufferSize: Int,
        headerSize: Int,
        width: Int,
        height: Int,
        fps: Int
    ): Boolean
    
    external fun stopVideo()
    external fun isVideoRunning(): Boolean
    external fun setVideoPattern(pattern: Int)
    
    // ========== Legacy (direct socket) ==========
    // These are kept for testing without the service
    
    external fun startAudio(frequency: Float, amplitude: Float): Boolean
    external fun startVideo(width: Int, height: Int, fps: Int): Boolean
    
    // ========== Preview Surface ==========
    
    /**
     * Set the preview surface where frames will also be rendered.
     * This allows seeing the exact frames being sent to the HAL.
     */
    external fun setPreviewSurface(surface: android.view.Surface?)
    
    // ========== Pattern constants ==========
    
    object Pattern {
        const val COLOR_BARS = 0
        const val GRADIENT = 1
        const val CHECKERBOARD = 2
        const val BOUNCING_BALL = 3
    }
}
