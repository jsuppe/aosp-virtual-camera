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
import android.util.Size
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {
    companion object {
        private const val TAG = "VCamTest"
        private const val VIRTUAL_CAMERA_ID = "100"  // Our virtual camera
        private const val PERMISSION_REQUEST = 100
    }

    private lateinit var cameraManager: CameraManager
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var imageReader: ImageReader? = null
    
    private lateinit var backgroundThread: HandlerThread
    private lateinit var backgroundHandler: Handler
    
    private lateinit var statusText: TextView
    private lateinit var surfaceView: SurfaceView
    private var frameCount = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        // Simple layout
        surfaceView = SurfaceView(this)
        setContentView(surfaceView)
        
        statusText = TextView(this).apply {
            text = "Virtual Camera Test"
            textSize = 16f
            setBackgroundColor(0x80000000.toInt())
            setTextColor(0xFFFFFFFF.toInt())
            setPadding(16, 16, 16, 16)
        }
        addContentView(statusText, android.widget.FrameLayout.LayoutParams(
            android.widget.FrameLayout.LayoutParams.WRAP_CONTENT,
            android.widget.FrameLayout.LayoutParams.WRAP_CONTENT
        ))
        
        cameraManager = getSystemService(CAMERA_SERVICE) as CameraManager
        
        // List all cameras
        try {
            val cameraIds = cameraManager.cameraIdList
            Log.i(TAG, "Available cameras: ${cameraIds.joinToString()}")
            statusText.text = "Cameras: ${cameraIds.joinToString()}"
        } catch (e: Exception) {
            Log.e(TAG, "Failed to list cameras", e)
        }
        
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Log.d(TAG, "Surface created")
                checkPermissionAndOpenCamera()
            }
            
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                Log.d(TAG, "Surface changed: ${width}x${height}")
            }
            
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                Log.d(TAG, "Surface destroyed")
                closeCamera()
            }
        })
    }
    
    override fun onResume() {
        super.onResume()
        startBackgroundThread()
    }
    
    override fun onPause() {
        closeCamera()
        stopBackgroundThread()
        super.onPause()
    }
    
    private fun startBackgroundThread() {
        backgroundThread = HandlerThread("CameraBackground").also { it.start() }
        backgroundHandler = Handler(backgroundThread.looper)
    }
    
    private fun stopBackgroundThread() {
        backgroundThread.quitSafely()
        backgroundThread.join()
    }
    
    private fun checkPermissionAndOpenCamera() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) 
            != PackageManager.PERMISSION_GRANTED) {
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
        }
    }
    
    private fun openVirtualCamera() {
        Log.i(TAG, "Opening virtual camera: $VIRTUAL_CAMERA_ID")
        statusText.text = "Opening camera $VIRTUAL_CAMERA_ID..."
        
        try {
            // Check if camera 100 exists
            val ids = cameraManager.cameraIdList
            if (!ids.contains(VIRTUAL_CAMERA_ID)) {
                Log.e(TAG, "Virtual camera $VIRTUAL_CAMERA_ID not found! Available: ${ids.joinToString()}")
                statusText.text = "Camera $VIRTUAL_CAMERA_ID not found!\nAvailable: ${ids.joinToString()}"
                return
            }
            
            // Get camera characteristics
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
            statusText.text = "Error: ${e.message}"
        }
    }
    
    private val stateCallback = object : CameraDevice.StateCallback() {
        override fun onOpened(camera: CameraDevice) {
            Log.i(TAG, "Camera opened!")
            cameraDevice = camera
            runOnUiThread { statusText.text = "Camera $VIRTUAL_CAMERA_ID opened!" }
            createCaptureSession()
        }
        
        override fun onDisconnected(camera: CameraDevice) {
            Log.w(TAG, "Camera disconnected")
            camera.close()
            cameraDevice = null
        }
        
        override fun onError(camera: CameraDevice, error: Int) {
            Log.e(TAG, "Camera error: $error")
            runOnUiThread { statusText.text = "Camera error: $error" }
            camera.close()
            cameraDevice = null
        }
    }
    
    private fun createCaptureSession() {
        val camera = cameraDevice ?: return
        
        try {
            // Create ImageReader for frame callback
            imageReader = ImageReader.newInstance(640, 480, ImageFormat.YUV_420_888, 2).apply {
                setOnImageAvailableListener({ reader ->
                    val image = reader.acquireLatestImage()
                    if (image != null) {
                        frameCount++
                        if (frameCount % 30 == 0) {
                            Log.i(TAG, "Received frame $frameCount")
                            runOnUiThread { 
                                statusText.text = "Frames: $frameCount" 
                            }
                        }
                        image.close()
                    }
                }, backgroundHandler)
            }
            
            val surface = surfaceView.holder.surface
            val surfaces = listOf(surface, imageReader!!.surface)
            
            camera.createCaptureSession(surfaces, object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    Log.i(TAG, "Capture session configured")
                    captureSession = session
                    startPreview()
                }
                
                override fun onConfigureFailed(session: CameraCaptureSession) {
                    Log.e(TAG, "Capture session configuration failed")
                    runOnUiThread { statusText.text = "Session config failed" }
                }
            }, backgroundHandler)
        } catch (e: CameraAccessException) {
            Log.e(TAG, "Failed to create session", e)
        }
    }
    
    private fun startPreview() {
        val camera = cameraDevice ?: return
        val session = captureSession ?: return
        
        try {
            val previewRequest = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                addTarget(surfaceView.holder.surface)
                addTarget(imageReader!!.surface)
            }.build()
            
            session.setRepeatingRequest(previewRequest, object : CameraCaptureSession.CaptureCallback() {
                override fun onCaptureCompleted(session: CameraCaptureSession, 
                                                request: CaptureRequest, 
                                                result: TotalCaptureResult) {
                    // Frame captured
                }
            }, backgroundHandler)
            
            Log.i(TAG, "Preview started!")
            runOnUiThread { statusText.text = "Preview started" }
        } catch (e: CameraAccessException) {
            Log.e(TAG, "Failed to start preview", e)
        }
    }
    
    private fun closeCamera() {
        captureSession?.close()
        captureSession = null
        cameraDevice?.close()
        cameraDevice = null
        imageReader?.close()
        imageReader = null
        Log.i(TAG, "Camera closed")
    }
}
