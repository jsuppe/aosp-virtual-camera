package com.example.vcamrenderer

import android.os.Bundle
import android.util.Log
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

/**
 * Golden Cube Virtual Camera Renderer
 * 
 * Preview Mode: Renders a rotating golden cube via Vulkan to a local SurfaceView.
 * This validates the Vulkan rendering pipeline works before integrating with
 * the VirtualCameraService.
 */
class MainActivity : AppCompatActivity() {
    
    companion object {
        private const val TAG = "VCamRenderer"
        
        init {
            System.loadLibrary("vcamrenderer")
        }
    }
    
    private var isRendering = false
    
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
    private lateinit var surfaceView: SurfaceView
    
    private var surfaceReady = false
    private var surfaceWidth = 0
    private var surfaceHeight = 0
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        
        statusText = findViewById(R.id.statusText)
        startButton = findViewById(R.id.startButton)
        surfaceView = findViewById(R.id.previewSurface)
        
        startButton.setOnClickListener {
            if (isRendering) {
                stopRendering()
            } else {
                startPreview()
            }
        }
        
        // Setup preview surface
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Log.d(TAG, "Preview surface created")
            }
            
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                Log.d(TAG, "Preview surface changed: ${width}x${height}")
                surfaceWidth = width
                surfaceHeight = height
                surfaceReady = true
                
                // If we were waiting to start, start now
                if (nativeContext != 0L && !isRendering) {
                    statusText.text = "Ready - tap Start to render\n${width}x${height}"
                }
            }
            
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                Log.d(TAG, "Preview surface destroyed")
                surfaceReady = false
                if (isRendering) {
                    stopRendering()
                }
            }
        })
        
        // Initialize Vulkan
        statusText.text = "Initializing Vulkan..."
        
        Thread {
            nativeContext = nativeInit()
            runOnUiThread {
                if (nativeContext == 0L) {
                    statusText.text = "❌ Failed to initialize Vulkan"
                    startButton.isEnabled = false
                } else {
                    statusText.text = "✓ Vulkan ready\nWaiting for surface..."
                    Log.i(TAG, "Vulkan initialized successfully")
                }
            }
        }.start()
    }
    
    override fun onDestroy() {
        super.onDestroy()
        stopRendering()
        if (nativeContext != 0L) {
            nativeDestroy(nativeContext)
            nativeContext = 0
        }
    }
    
    private fun startPreview() {
        if (!surfaceReady) {
            statusText.text = "Surface not ready yet"
            return
        }
        
        if (nativeContext == 0L) {
            statusText.text = "Vulkan not initialized"
            return
        }
        
        val surface = surfaceView.holder.surface
        if (!surface.isValid) {
            statusText.text = "Surface not valid"
            return
        }
        
        Log.d(TAG, "Starting preview: ${surfaceWidth}x${surfaceHeight}")
        statusText.text = "Setting up swapchain..."
        
        Thread {
            val success = nativeSetSurface(nativeContext, surface, surfaceWidth, surfaceHeight)
            runOnUiThread {
                if (success) {
                    nativeStartRendering(nativeContext)
                    isRendering = true
                    startButton.text = "Stop"
                    statusText.text = "🎲 Rendering cube\n${surfaceWidth}x${surfaceHeight} @ 60fps"
                } else {
                    statusText.text = "❌ Failed to create swapchain"
                }
            }
        }.start()
    }
    
    private fun stopRendering() {
        if (nativeContext != 0L) {
            nativeStopRendering(nativeContext)
        }
        isRendering = false
        startButton.text = "Start"
        statusText.text = "Stopped"
    }
}
