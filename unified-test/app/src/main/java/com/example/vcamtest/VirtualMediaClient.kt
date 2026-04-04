package com.example.vcamtest

import android.hardware.HardwareBuffer
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.util.Log
import android.view.Surface
import java.lang.reflect.Method

/**
 * Client for VirtualMediaService system service.
 * Uses reflection since the API is @hide.
 */
class VirtualMediaClient {
    
    companion object {
        private const val TAG = "VirtualMediaClient"
        private const val SERVICE_NAME = "virtual_media"
    }
    
    private var service: Any? = null
    private var startAudioMethod: Method? = null
    private var stopAudioMethod: Method? = null
    private var startVideoMethod: Method? = null
    private var stopVideoMethod: Method? = null
    private var sendHardwareBufferMethod: Method? = null
    private var isAudioConnectedMethod: Method? = null
    private var isVideoConnectedMethod: Method? = null
    private var createVideoSurfaceMethod: Method? = null
    private var releaseVideoSurfaceMethod: Method? = null
    
    data class RendererInfo(
        val id: Int,
        val bufferFd: ParcelFileDescriptor?,
        val bufferSize: Int,
        val headerSize: Int
    )
    
    data class AudioConfig(
        val sampleRate: Int = 48000,
        val channelCount: Int = 1,  // Mono for mic
        val format: Int = 1,  // PCM_16BIT
        val bufferFrames: Int = 4096
    )
    
    data class VideoConfig(
        val width: Int = 1280,
        val height: Int = 720,
        val fps: Int = 30,
        val format: Int = 1  // RGBA8888
    )
    
    fun connect(): Boolean {
        try {
            // Get ServiceManager class
            val smClass = Class.forName("android.os.ServiceManager")
            val getService = smClass.getMethod("getService", String::class.java)
            
            val binder = getService.invoke(null, SERVICE_NAME) as? IBinder
            if (binder == null) {
                Log.e(TAG, "VirtualMediaService not found")
                return false
            }
            
            // Get the stub class and asInterface method
            val stubClass = Class.forName("android.virtualmedia.IVirtualMediaService\$Stub")
            val asInterface = stubClass.getMethod("asInterface", IBinder::class.java)
            service = asInterface.invoke(null, binder)
            
            if (service == null) {
                Log.e(TAG, "Failed to get service interface")
                return false
            }
            
            // Cache methods for later calls
            val serviceClass = service!!.javaClass
            
            // Get AIDL parcelable classes
            val audioConfigClass = Class.forName("android.virtualmedia.AudioConfig")
            val videoConfigClass = Class.forName("android.virtualmedia.VideoConfig")
            
            startAudioMethod = serviceClass.getMethod("startAudioRenderer", audioConfigClass)
            stopAudioMethod = serviceClass.getMethod("stopAudioRenderer", Int::class.java)
            startVideoMethod = serviceClass.getMethod("startVideoRenderer", videoConfigClass)
            stopVideoMethod = serviceClass.getMethod("stopVideoRenderer", Int::class.java)
            sendHardwareBufferMethod = serviceClass.getMethod("sendHardwareBuffer", 
                Int::class.java, HardwareBuffer::class.java, Long::class.java)
            isAudioConnectedMethod = serviceClass.getMethod("isAudioHalConnected")
            isVideoConnectedMethod = serviceClass.getMethod("isVideoHalConnected")
            
            // Try to get Surface methods (may not exist on older builds)
            try {
                createVideoSurfaceMethod = serviceClass.getMethod("createVideoSurface",
                    Int::class.java, Int::class.java)
                releaseVideoSurfaceMethod = serviceClass.getMethod("releaseVideoSurface")
                Log.i(TAG, "Surface API available")
            } catch (e: NoSuchMethodException) {
                Log.w(TAG, "Surface API not available, using shared memory only")
            }
            
            Log.i(TAG, "Connected to VirtualMediaService")
            return true
            
        } catch (e: Exception) {
            Log.e(TAG, "Failed to connect: ${e.message}", e)
            return false
        }
    }
    
    fun isAudioHalConnected(): Boolean {
        return try {
            isAudioConnectedMethod?.invoke(service) as? Boolean ?: false
        } catch (e: Exception) {
            Log.e(TAG, "isAudioHalConnected failed: ${e.message}")
            false
        }
    }
    
    fun isVideoHalConnected(): Boolean {
        return try {
            isVideoConnectedMethod?.invoke(service) as? Boolean ?: false
        } catch (e: Exception) {
            Log.e(TAG, "isVideoHalConnected failed: ${e.message}")
            false
        }
    }
    
    fun startAudioRenderer(config: AudioConfig = AudioConfig()): RendererInfo? {
        try {
            // Create AIDL AudioConfig object
            val aidlConfigClass = Class.forName("android.virtualmedia.AudioConfig")
            val aidlConfig = aidlConfigClass.newInstance()
            
            aidlConfigClass.getField("sampleRate").setInt(aidlConfig, config.sampleRate)
            aidlConfigClass.getField("channelCount").setInt(aidlConfig, config.channelCount)
            aidlConfigClass.getField("format").setInt(aidlConfig, config.format)
            aidlConfigClass.getField("bufferFrames").setInt(aidlConfig, config.bufferFrames)
            
            val result = startAudioMethod?.invoke(service, aidlConfig) ?: return null
            
            return parseRendererInfo(result)
            
        } catch (e: Exception) {
            Log.e(TAG, "startAudioRenderer failed: ${e.message}", e)
            return null
        }
    }
    
    fun stopAudioRenderer(rendererId: Int) {
        try {
            stopAudioMethod?.invoke(service, rendererId)
        } catch (e: Exception) {
            Log.e(TAG, "stopAudioRenderer failed: ${e.message}")
        }
    }
    
    fun startVideoRenderer(config: VideoConfig = VideoConfig()): RendererInfo? {
        try {
            // Create AIDL VideoConfig object
            val aidlConfigClass = Class.forName("android.virtualmedia.VideoConfig")
            val aidlConfig = aidlConfigClass.newInstance()
            
            aidlConfigClass.getField("width").setInt(aidlConfig, config.width)
            aidlConfigClass.getField("height").setInt(aidlConfig, config.height)
            aidlConfigClass.getField("fps").setInt(aidlConfig, config.fps)
            aidlConfigClass.getField("format").setInt(aidlConfig, config.format)
            
            val result = startVideoMethod?.invoke(service, aidlConfig) ?: return null
            
            return parseRendererInfo(result)
            
        } catch (e: Exception) {
            Log.e(TAG, "startVideoRenderer failed: ${e.message}", e)
            return null
        }
    }
    
    fun stopVideoRenderer(rendererId: Int) {
        try {
            stopVideoMethod?.invoke(service, rendererId)
        } catch (e: Exception) {
            Log.e(TAG, "stopVideoRenderer failed: ${e.message}")
        }
    }
    
    /**
     * Send a HardwareBuffer frame directly to the virtual camera (zero-copy path).
     * This bypasses the shared memory buffer for lower latency.
     * 
     * @param rendererId The renderer ID from startVideoRenderer()
     * @param buffer The HardwareBuffer containing the frame (RGBA format)
     * @param timestampNs Frame timestamp in nanoseconds
     * @return true if successful
     */
    fun sendHardwareBuffer(rendererId: Int, buffer: HardwareBuffer, timestampNs: Long): Boolean {
        return try {
            sendHardwareBufferMethod?.invoke(service, rendererId, buffer, timestampNs)
            true
        } catch (e: Exception) {
            Log.e(TAG, "sendHardwareBuffer failed: ${e.message}", e)
            false
        }
    }
    
    /**
     * Create a Surface for zero-copy video rendering.
     * Apps render directly to this Surface using OpenGL/Vulkan/Canvas.
     * Frames are automatically captured and forwarded to the Camera HAL.
     * 
     * @param width Width of the video frames
     * @param height Height of the video frames
     * @return A Surface to render to, or null if creation failed
     */
    fun createVideoSurface(width: Int, height: Int): Surface? {
        return try {
            val result = createVideoSurfaceMethod?.invoke(service, width, height)
            result as? Surface
        } catch (e: Exception) {
            Log.e(TAG, "createVideoSurface failed: ${e.message}", e)
            null
        }
    }
    
    /**
     * Release the video Surface and associated resources.
     */
    fun releaseVideoSurface() {
        try {
            releaseVideoSurfaceMethod?.invoke(service)
        } catch (e: Exception) {
            Log.e(TAG, "releaseVideoSurface failed: ${e.message}")
        }
    }
    
    private fun parseRendererInfo(result: Any): RendererInfo? {
        return try {
            val resultClass = result.javaClass
            val id = resultClass.getField("id").getInt(result)
            val buffer = resultClass.getField("buffer").get(result) as? ParcelFileDescriptor
            val bufferSize = resultClass.getField("bufferSize").getInt(result)
            val headerSize = resultClass.getField("headerSize").getInt(result)
            
            RendererInfo(id, buffer, bufferSize, headerSize)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to parse RendererInfo: ${e.message}")
            null
        }
    }
}
