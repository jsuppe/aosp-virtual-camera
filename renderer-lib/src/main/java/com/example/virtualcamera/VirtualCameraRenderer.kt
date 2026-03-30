package com.example.virtualcamera

import java.nio.ByteBuffer

/**
 * Client library for providing frames to the virtual camera HAL.
 * 
 * Usage:
 * ```
 * val renderer = VirtualCameraRenderer()
 * renderer.initialize(1920, 1080, PixelFormat.YUV_420_888)
 * 
 * // In render loop:
 * val buffer = renderer.beginFrame()
 * if (buffer != null) {
 *     // Write frame data to buffer
 *     writeYuvData(buffer)
 *     renderer.endFrame()
 * }
 * 
 * // When done:
 * renderer.shutdown()
 * ```
 */
class VirtualCameraRenderer {
    
    companion object {
        init {
            System.loadLibrary("virtualcamerarenderer")
        }
    }
    
    enum class PixelFormat(val value: Int) {
        RGBA_8888(1),
        YUV_420_888(35),
        IMPLEMENTATION_DEFINED(34)
    }
    
    /**
     * Initialize the renderer with specified dimensions and format.
     * @return true if successful
     */
    fun initialize(width: Int, height: Int, format: PixelFormat = PixelFormat.YUV_420_888): Boolean {
        return nativeInitialize(width, height, format.value)
    }
    
    /**
     * Shutdown and release resources.
     */
    fun shutdown() {
        nativeShutdown()
    }
    
    /**
     * Check if renderer is initialized and ready.
     */
    fun isReady(): Boolean = nativeIsReady()
    
    /**
     * Begin writing a new frame.
     * @return Direct ByteBuffer for writing frame data, or null if not ready
     */
    fun beginFrame(): ByteBuffer? = nativeBeginFrame()
    
    /**
     * Signal that the current frame is complete.
     * @param timestamp Frame timestamp in nanoseconds (0 = auto-generate)
     */
    fun endFrame(timestamp: Long = 0) {
        nativeEndFrame(timestamp)
    }
    
    /**
     * Get frame width in pixels.
     */
    val width: Int get() = nativeGetWidth()
    
    /**
     * Get frame height in pixels.
     */
    val height: Int get() = nativeGetHeight()
    
    /**
     * Get frame buffer size in bytes.
     */
    val frameSize: Int get() = nativeGetFrameSize()
    
    // Native methods
    private external fun nativeInitialize(width: Int, height: Int, format: Int): Boolean
    private external fun nativeShutdown()
    private external fun nativeIsReady(): Boolean
    private external fun nativeBeginFrame(): ByteBuffer?
    private external fun nativeEndFrame(timestamp: Long)
    private external fun nativeGetWidth(): Int
    private external fun nativeGetHeight(): Int
    private external fun nativeGetFrameSize(): Int
}
