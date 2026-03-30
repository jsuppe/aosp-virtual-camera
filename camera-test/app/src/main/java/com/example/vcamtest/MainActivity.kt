package com.example.vcamtest

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.hardware.camera2.*
import android.media.ImageReader
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {
    companion object {
        private const val TAG = "VCamTest"
        private const val VIRTUAL_CAMERA_ID = "100"
        private const val PERMISSION_REQUEST = 100
    }

    private lateinit var cameraManager: CameraManager
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var imageReader: ImageReader? = null
    
    private var backgroundThread: HandlerThread? = null
    private var backgroundHandler: Handler? = null
    
    private lateinit var statusText: TextView
    private lateinit var surfaceView: SurfaceView
    private var frameCount = 0
    
    // Track state to prevent race conditions
    private var surfaceReady = false
    private var cameraOpening = false
    private var isResumed = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        // Keep screen on to prevent surface destruction
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        
        // Create layout
        val layout = FrameLayout(this)
        
        surfaceView = SurfaceView(this)
        layout.addView(surfaceView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT
        ))
        
        statusText = TextView(this).apply {
            text = "Virtual Camera Test\nInitializing..."
            textSize = 18f
            setBackgroundColor(0xCC000000.toInt())
            setTextColor(0xFFFFFFFF.toInt())
            setPadding(24, 24, 24, 24)
        }
        layout.addView(statusText, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT
        ))
        
        setContentView(layout)
        
        cameraManager = getSystemService(CAMERA_SERVICE) as CameraManager
        
        // List all cameras
        try {
            val cameraIds = cameraManager.cameraIdList
            Log.i(TAG, "Available cameras: ${cameraIds.joinToString()}")
            updateStatus("Cameras: ${cameraIds.joinToString()}\nWaiting for surface...")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to list cameras", e)
            updateStatus("Error listing cameras: ${e.message}")
        }
        
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Log.d(TAG, "Surface created")
                surfaceReady = true
                if (isResumed) {
                    checkPermissionAndOpenCamera()
                }
            }
            
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                Log.d(TAG, "Surface changed: ${width}x${height}")
            }
            
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                Log.d(TAG, "Surface destroyed")
                surfaceReady = false
                closeCamera()
            }
        })
    }
    
    private fun updateStatus(text: String) {
        runOnUiThread {
            statusText.text = text
        }
    }
    
    override fun onResume() {
        super.onResume()
        Log.d(TAG, "onResume")
        isResumed = true
        startBackgroundThread()
        
        // If surface is already ready, open camera
        if (surfaceReady && cameraDevice == null && !cameraOpening) {
            checkPermissionAndOpenCamera()
        }
    }
    
    override fun onPause() {
        Log.d(TAG, "onPause")
        isResumed = false
        closeCamera()
        stopBackgroundThread()
        super.onPause()
    }
    
    private fun startBackgroundThread() {
        if (backgroundThread == null) {
            backgroundThread = HandlerThread("CameraBackground").also { it.start() }
            backgroundHandler = Handler(backgroundThread!!.looper)
        }
    }
    
    private fun stopBackgroundThread() {
        backgroundThread?.quitSafely()
        try {
            backgroundThread?.join()
        } catch (e: InterruptedException) {
            Log.e(TAG, "Error stopping background thread", e)
        }
        backgroundThread = null
        backgroundHandler = null
    }
    
    private fun checkPermissionAndOpenCamera() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) 
            != PackageManager.PERMISSION_GRANTED) {
            updateStatus("Requesting camera permission...")
            ActivityCompat.requestPermissions(this, 
                arrayOf(Manifest.permission.CAMERA), PERMISSION_REQUEST)
        } else {
            openVirtualCamera()
        }
    }
    
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<String>, 
                                            grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == PERMISSION_REQUEST && grantResults.isNotEmpty() 
            && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            openVirtualCamera()
        } else {
            Toast.makeText(this, "Camera permission required", Toast.LENGTH_LONG).show()
            updateStatus("Camera permission denied")
        }
    }
    
    private fun openVirtualCamera() {
        if (cameraOpening || cameraDevice != null) {
            Log.d(TAG, "Camera already opening or opened")
            return
        }
        
        cameraOpening = true
        Log.i(TAG, "Opening virtual camera: $VIRTUAL_CAMERA_ID")
        updateStatus("Opening camera $VIRTUAL_CAMERA_ID...")
        
        try {
            val ids = cameraManager.cameraIdList
            if (!ids.contains(VIRTUAL_CAMERA_ID)) {
                Log.e(TAG, "Virtual camera $VIRTUAL_CAMERA_ID not found!")
                updateStatus("Camera $VIRTUAL_CAMERA_ID not found!\nAvailable: ${ids.joinToString()}")
                cameraOpening = false
                return
            }
            
            // Log available sizes
            val chars = cameraManager.getCameraCharacteristics(VIRTUAL_CAMERA_ID)
            val configs = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            val sizes = configs?.getOutputSizes(SurfaceHolder::class.java)
            Log.i(TAG, "Available sizes: ${sizes?.joinToString()}")
            
            if (ActivityCompat.checkSelfPermission(this, Manifest.permission.CAMERA) 
                == PackageManager.PERMISSION_GRANTED) {
                cameraManager.openCamera(VIRTUAL_CAMERA_ID, stateCallback, backgroundHandler)
            }
        } catch (e: CameraAccessException) {
            Log.e(TAG, "Failed to open camera", e)
            updateStatus("Error: ${e.message}")
            cameraOpening = false
        }
    }
    
    private val stateCallback = object : CameraDevice.StateCallback() {
        override fun onOpened(camera: CameraDevice) {
            Log.i(TAG, "Camera opened!")
            cameraDevice = camera
            cameraOpening = false
            updateStatus("Camera $VIRTUAL_CAMERA_ID opened!\nCreating session...")
            createCaptureSession()
        }
        
        override fun onDisconnected(camera: CameraDevice) {
            Log.w(TAG, "Camera disconnected")
            cameraOpening = false
            camera.close()
            cameraDevice = null
            updateStatus("Camera disconnected")
        }
        
        override fun onError(camera: CameraDevice, error: Int) {
            Log.e(TAG, "Camera error: $error")
            cameraOpening = false
            updateStatus("Camera error: $error")
            camera.close()
            cameraDevice = null
        }
    }
    
    private fun createCaptureSession() {
        val camera = cameraDevice ?: return
        if (!surfaceReady) {
            Log.w(TAG, "Surface not ready, cannot create session")
            updateStatus("Waiting for surface...")
            return
        }
        
        try {
            // Use 640x480 for compatibility
            imageReader = ImageReader.newInstance(640, 480, ImageFormat.YUV_420_888, 3).apply {
                setOnImageAvailableListener({ reader ->
                    val image = reader.acquireLatestImage()
                    if (image != null) {
                        frameCount++
                        if (frameCount % 30 == 0) {
                            Log.i(TAG, "Received frame $frameCount (${image.width}x${image.height})")
                            updateStatus("Camera $VIRTUAL_CAMERA_ID active\nFrames: $frameCount")
                        }
                        image.close()
                    }
                }, backgroundHandler)
            }
            
            val surface = surfaceView.holder.surface
            val surfaces = listOf(imageReader!!.surface)  // Just use ImageReader for now
            
            camera.createCaptureSession(surfaces, object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    Log.i(TAG, "Capture session configured")
                    captureSession = session
                    updateStatus("Session configured!\nStarting preview...")
                    startPreview()
                }
                
                override fun onConfigureFailed(session: CameraCaptureSession) {
                    Log.e(TAG, "Capture session configuration failed")
                    updateStatus("Session config failed")
                }
            }, backgroundHandler)
        } catch (e: CameraAccessException) {
            Log.e(TAG, "Failed to create session", e)
            updateStatus("Session error: ${e.message}")
        }
    }
    
    private fun startPreview() {
        val camera = cameraDevice ?: return
        val session = captureSession ?: return
        
        try {
            val previewRequest = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                addTarget(imageReader!!.surface)
            }.build()
            
            session.setRepeatingRequest(previewRequest, object : CameraCaptureSession.CaptureCallback() {
                override fun onCaptureCompleted(session: CameraCaptureSession, 
                                                request: CaptureRequest, 
                                                result: TotalCaptureResult) {
                    // Frame captured successfully
                }
                
                override fun onCaptureFailed(session: CameraCaptureSession,
                                            request: CaptureRequest,
                                            failure: CaptureFailure) {
                    Log.w(TAG, "Capture failed: ${failure.reason}")
                }
            }, backgroundHandler)
            
            Log.i(TAG, "Preview started!")
            updateStatus("Preview started!\nReceiving frames...")
        } catch (e: CameraAccessException) {
            Log.e(TAG, "Failed to start preview", e)
            updateStatus("Preview error: ${e.message}")
        }
    }
    
    private fun closeCamera() {
        Log.i(TAG, "Closing camera...")
        captureSession?.close()
        captureSession = null
        cameraDevice?.close()
        cameraDevice = null
        imageReader?.close()
        imageReader = null
        cameraOpening = false
        frameCount = 0
        Log.i(TAG, "Camera closed")
    }
}
