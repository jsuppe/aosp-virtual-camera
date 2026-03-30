package com.example.vcamtest

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.SurfaceTexture
import android.hardware.camera2.*
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Size
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.TextureView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat

/**
 * Unified test app for virtual camera pipeline.
 * 
 * Left side: Vulkan renders golden cube directly
 * Right side: Same cube received via Camera2 API from virtual camera HAL
 * 
 * If both match → pipeline works!
 */
class MainActivity : AppCompatActivity() {
    
    companion object {
        private const val TAG = "VCamTest"
        private const val VIRTUAL_CAMERA_ID = "100"  // Our virtual camera
        private const val REQUEST_CAMERA_PERMISSION = 1
    }
    
    // Views
    private lateinit var rendererView: SurfaceView      // Left: Vulkan direct
    private lateinit var cameraView: TextureView        // Right: Camera2 preview
    private lateinit var statusText: TextView
    
    // Camera2
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null
    
    // Native renderer
    private var nativeRenderer: Long = 0
    private var frameCount = 0
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        
        rendererView = findViewById(R.id.renderer_view)
        cameraView = findViewById(R.id.camera_view)
        statusText = findViewById(R.id.status_text)
        
        // Set up renderer surface
        rendererView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Log.i(TAG, "Renderer surface created")
                startRenderer(holder.surface)
            }
            
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                Log.i(TAG, "Renderer surface changed: ${width}x${height}")
            }
            
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                Log.i(TAG, "Renderer surface destroyed")
                stopRenderer()
            }
        })
        
        // Set up camera preview surface
        cameraView.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                Log.i(TAG, "Camera surface available: ${width}x${height}")
                checkPermissionAndOpenCamera()
            }
            
            override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {}
            override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                closeCamera()
                return true
            }
            override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {}
        }
        
        updateStatus("Initializing...")
    }
    
    private fun startRenderer(surface: Surface) {
        // Start native Vulkan renderer
        nativeRenderer = nativeCreateRenderer(surface)
        if (nativeRenderer != 0L) {
            Log.i(TAG, "Renderer created: $nativeRenderer")
            // Start render loop in background thread
            Thread {
                while (nativeRenderer != 0L) {
                    nativeRenderFrame(nativeRenderer)
                    frameCount++
                    if (frameCount % 60 == 0) {
                        runOnUiThread { updateStatus("Renderer: $frameCount frames") }
                    }
                    Thread.sleep(16) // ~60 FPS
                }
            }.start()
        } else {
            Log.e(TAG, "Failed to create renderer")
            updateStatus("Renderer failed!")
        }
    }
    
    private fun stopRenderer() {
        if (nativeRenderer != 0L) {
            nativeDestroyRenderer(nativeRenderer)
            nativeRenderer = 0
        }
    }
    
    private fun checkPermissionAndOpenCamera() {
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.CAMERA), REQUEST_CAMERA_PERMISSION)
        } else {
            openVirtualCamera()
        }
    }
    
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_CAMERA_PERMISSION && grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            openVirtualCamera()
        } else {
            updateStatus("Camera permission denied")
        }
    }
    
    private fun openVirtualCamera() {
        cameraThread = HandlerThread("CameraThread").also { it.start() }
        cameraHandler = Handler(cameraThread!!.looper)
        
        val cameraManager = getSystemService(CAMERA_SERVICE) as CameraManager
        
        // List cameras to find our virtual camera
        val cameraIds = cameraManager.cameraIdList
        Log.i(TAG, "Available cameras: ${cameraIds.joinToString()}")
        
        // Find camera ID 100 or fall back to last camera
        val targetId = if (cameraIds.contains(VIRTUAL_CAMERA_ID)) {
            VIRTUAL_CAMERA_ID
        } else {
            cameraIds.lastOrNull() ?: run {
                updateStatus("No cameras found!")
                return
            }
        }
        
        Log.i(TAG, "Opening camera: $targetId")
        updateStatus("Opening camera $targetId...")
        
        try {
            if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
                cameraManager.openCamera(targetId, object : CameraDevice.StateCallback() {
                    override fun onOpened(camera: CameraDevice) {
                        Log.i(TAG, "Camera opened: ${camera.id}")
                        cameraDevice = camera
                        createPreviewSession()
                    }
                    
                    override fun onDisconnected(camera: CameraDevice) {
                        Log.w(TAG, "Camera disconnected")
                        cameraDevice = null
                    }
                    
                    override fun onError(camera: CameraDevice, error: Int) {
                        Log.e(TAG, "Camera error: $error")
                        updateStatus("Camera error: $error")
                    }
                }, cameraHandler)
            }
        } catch (e: CameraAccessException) {
            Log.e(TAG, "Failed to open camera", e)
            updateStatus("Camera access error")
        }
    }
    
    private fun createPreviewSession() {
        val camera = cameraDevice ?: return
        val surfaceTexture = cameraView.surfaceTexture ?: return
        
        // Set buffer size
        surfaceTexture.setDefaultBufferSize(640, 480)
        val surface = Surface(surfaceTexture)
        
        try {
            val previewRequest = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                addTarget(surface)
            }
            
            camera.createCaptureSession(listOf(surface), object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    Log.i(TAG, "Preview session configured")
                    captureSession = session
                    session.setRepeatingRequest(previewRequest.build(), null, cameraHandler)
                    runOnUiThread { updateStatus("Camera active! Renderer: $frameCount frames") }
                }
                
                override fun onConfigureFailed(session: CameraCaptureSession) {
                    Log.e(TAG, "Preview session configuration failed")
                    runOnUiThread { updateStatus("Preview config failed") }
                }
            }, cameraHandler)
        } catch (e: CameraAccessException) {
            Log.e(TAG, "Failed to create preview session", e)
        }
    }
    
    private fun closeCamera() {
        captureSession?.close()
        captureSession = null
        cameraDevice?.close()
        cameraDevice = null
        cameraThread?.quitSafely()
        cameraThread = null
    }
    
    private fun updateStatus(text: String) {
        statusText.text = text
    }
    
    override fun onDestroy() {
        super.onDestroy()
        stopRenderer()
        closeCamera()
    }
    
    // Native methods
    private external fun nativeCreateRenderer(surface: Surface): Long
    private external fun nativeRenderFrame(renderer: Long)
    private external fun nativeDestroyRenderer(renderer: Long)
    
    companion object {
        init {
            System.loadLibrary("vcam_unified_test")
        }
    }
}
