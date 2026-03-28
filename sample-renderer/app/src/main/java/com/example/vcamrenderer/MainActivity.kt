package com.example.vcamrenderer

import android.graphics.PixelFormat
import android.os.Bundle
import android.os.IBinder
import android.os.ServiceManager
import android.util.Log
import android.view.Surface
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

/**
 * Sample Virtual Camera Renderer
 * 
 * Renders a rotating golden cube via Vulkan and provides it
 * as a virtual camera source.
 */
class MainActivity : AppCompatActivity() {
    
    companion object {
        private const val TAG = "VCamRenderer"
        
        init {
            System.loadLibrary("vcamrenderer")
        }
    }
    
    private var isRendering = false
    private var cameraId = -1
    
    // Native methods
    private external fun nativeInit(): Long
    private external fun nativeDestroy(ctx: Long)
    private external fun nativeSetSurface(ctx: Long, surface: Surface, width: Int, height: Int): Boolean
    private external fun nativeStartRendering(ctx: Long)
    private external fun nativeStopRendering(ctx: Long)
    private external fun nativeSetRotation(ctx: Long, angleX: Float, angleY: Float)
    
    private var nativeContext: Long = 0
    
    private lateinit var statusText: TextView
    private lateinit var startButton: Button
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        
        statusText = findViewById(R.id.statusText)
        startButton = findViewById(R.id.startButton)
        
        startButton.setOnClickListener {
            if (isRendering) {
                stopCamera()
            } else {
                startCamera()
            }
        }
        
        // Initialize Vulkan
        nativeContext = nativeInit()
        if (nativeContext == 0L) {
            statusText.text = "Failed to initialize Vulkan"
            startButton.isEnabled = false
            return
        }
        
        statusText.text = "Ready - tap Start to begin"
    }
    
    override fun onDestroy() {
        super.onDestroy()
        stopCamera()
        if (nativeContext != 0L) {
            nativeDestroy(nativeContext)
            nativeContext = 0
        }
    }
    
    private fun startCamera() {
        try {
            // Get the VirtualCameraService
            val binder = ServiceManager.getService("virtual_camera")
            if (binder == null) {
                statusText.text = "VirtualCameraService not found"
                return
            }
            
            val service = IVirtualCameraService.Stub.asInterface(binder)
            
            // Configure camera: 4K60 RGBA
            val config = VirtualCameraConfig().apply {
                name = "Golden Cube Camera"
                maxWidth = 3840
                maxHeight = 2160
                maxFps = 60
                supportedFormats = intArrayOf(PixelFormat.RGBA_8888)
                facing = 2  // EXTERNAL
                orientation = 0
                supportsJpeg = false
            }
            
            // Register with callback
            cameraId = service.registerCamera(config, cameraCallback)
            
            if (cameraId < 0) {
                statusText.text = "Failed to register camera"
                return
            }
            
            isRendering = true
            startButton.text = "Stop"
            statusText.text = "Camera registered (ID: $cameraId)\nWaiting for client..."
            
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start camera", e)
            statusText.text = "Error: ${e.message}"
        }
    }
    
    private fun stopCamera() {
        if (cameraId >= 0) {
            try {
                val binder = ServiceManager.getService("virtual_camera")
                val service = IVirtualCameraService.Stub.asInterface(binder)
                service?.unregisterCamera(cameraId)
            } catch (e: Exception) {
                Log.e(TAG, "Error unregistering camera", e)
            }
            cameraId = -1
        }
        
        nativeStopRendering(nativeContext)
        isRendering = false
        startButton.text = "Start"
        statusText.text = "Stopped"
    }
    
    private val cameraCallback = object : IVirtualCameraCallback.Stub() {
        
        override fun onCameraOpened() {
            runOnUiThread {
                statusText.text = "Camera opened by client"
            }
        }
        
        override fun onStreamsConfigured(streams: Array<StreamConfig>, surfaces: Array<Surface>) {
            runOnUiThread {
                if (streams.isNotEmpty() && surfaces.isNotEmpty()) {
                    val stream = streams[0]
                    val surface = surfaces[0]
                    
                    statusText.text = "Streaming: ${stream.width}x${stream.height} @ ${stream.fps}fps"
                    
                    // Configure Vulkan to render to this surface
                    if (nativeSetSurface(nativeContext, surface, stream.width, stream.height)) {
                        nativeStartRendering(nativeContext)
                    } else {
                        statusText.text = "Failed to set surface"
                    }
                }
            }
        }
        
        override fun onCaptureStarted(frameRate: Int) {
            runOnUiThread {
                statusText.text = "Rendering at $frameRate fps"
            }
        }
        
        override fun onCaptureStopped() {
            runOnUiThread {
                nativeStopRendering(nativeContext)
                statusText.text = "Capture stopped"
            }
        }
        
        override fun onCameraClosed() {
            runOnUiThread {
                nativeStopRendering(nativeContext)
                statusText.text = "Camera closed - waiting for client..."
            }
        }
        
        override fun onStillCaptureRequested(surface: Surface, captureId: Int) {
            // Render a single high-quality frame
            Log.d(TAG, "Still capture requested: $captureId")
        }
    }
}
