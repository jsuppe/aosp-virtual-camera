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
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

class MainActivity : AppCompatActivity() {
    
    companion object {
        private const val TAG = "VCamTest"
        private const val TARGET_CAMERA_ID = "100"  // Our virtual camera
        private const val REQUEST_CAMERA_PERMISSION = 100
        private const val TEST_DURATION_MS = 5000L
        private const val WIDTH = 1280
        private const val HEIGHT = 720
    }
    
    private lateinit var statusText: TextView
    private lateinit var resultsText: TextView
    private lateinit var startButton: Button
    
    private var cameraManager: CameraManager? = null
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var imageReader: ImageReader? = null
    
    private var backgroundThread: HandlerThread? = null
    private var backgroundHandler: Handler? = null
    
    private val frameCount = AtomicInteger(0)
    private val firstFrameTime = AtomicLong(0)
    private val lastFrameTime = AtomicLong(0)
    private var testStartTime: Long = 0
    
    private val results = StringBuilder()
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        
        statusText = findViewById(R.id.statusText)
        resultsText = findViewById(R.id.resultsText)
        startButton = findViewById(R.id.startButton)
        
        cameraManager = getSystemService(CAMERA_SERVICE) as CameraManager
        
        startButton.setOnClickListener {
            if (checkCameraPermission()) {
                startTest()
            }
        }
        
        // List available cameras
        listCameras()
    }
    
    private fun listCameras() {
        try {
            val cameraIds = cameraManager?.cameraIdList ?: emptyArray()
            log("Available cameras: ${cameraIds.joinToString(", ")}")
            
            var foundVirtualCamera = false
            for (id in cameraIds) {
                val characteristics = cameraManager?.getCameraCharacteristics(id)
                val facing = characteristics?.get(CameraCharacteristics.LENS_FACING)
                val facingStr = when (facing) {
                    CameraCharacteristics.LENS_FACING_FRONT -> "FRONT"
                    CameraCharacteristics.LENS_FACING_BACK -> "BACK"
                    CameraCharacteristics.LENS_FACING_EXTERNAL -> "EXTERNAL"
                    else -> "UNKNOWN"
                }
                log("  Camera $id: $facingStr")
                
                if (id == TARGET_CAMERA_ID) {
                    foundVirtualCamera = true
                }
            }
            
            if (foundVirtualCamera) {
                log("✓ Virtual camera (ID $TARGET_CAMERA_ID) found!")
                updateStatus("Ready to test camera $TARGET_CAMERA_ID")
            } else {
                log("✗ Virtual camera (ID $TARGET_CAMERA_ID) NOT found!")
                updateStatus("ERROR: Camera $TARGET_CAMERA_ID not found")
            }
        } catch (e: Exception) {
            log("Error listing cameras: ${e.message}")
        }
    }
    
    private fun checkCameraPermission(): Boolean {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(
                this,
                arrayOf(Manifest.permission.CAMERA),
                REQUEST_CAMERA_PERMISSION
            )
            return false
        }
        return true
    }
    
    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_CAMERA_PERMISSION) {
            if (grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                startTest()
            } else {
                log("Camera permission denied")
            }
        }
    }
    
    private fun startTest() {
        startButton.isEnabled = false
        results.clear()
        frameCount.set(0)
        firstFrameTime.set(0)
        lastFrameTime.set(0)
        
        log("=== Starting Virtual Camera Test ===")
        log("Target: Camera ID $TARGET_CAMERA_ID")
        log("Resolution: ${WIDTH}x${HEIGHT}")
        log("Duration: ${TEST_DURATION_MS}ms")
        log("")
        
        startBackgroundThread()
        openCamera()
    }
    
    private fun startBackgroundThread() {
        backgroundThread = HandlerThread("CameraBackground").also { it.start() }
        backgroundHandler = Handler(backgroundThread!!.looper)
    }
    
    private fun stopBackgroundThread() {
        backgroundThread?.quitSafely()
        try {
            backgroundThread?.join()
            backgroundThread = null
            backgroundHandler = null
        } catch (e: InterruptedException) {
            Log.e(TAG, "Error stopping background thread", e)
        }
    }
    
    private fun openCamera() {
        try {
            updateStatus("Opening camera $TARGET_CAMERA_ID...")
            log("Opening camera...")
            
            if (ActivityCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
                != PackageManager.PERMISSION_GRANTED) {
                log("ERROR: No camera permission")
                return
            }
            
            cameraManager?.openCamera(TARGET_CAMERA_ID, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    log("✓ Camera opened successfully")
                    cameraDevice = camera
                    createCaptureSession()
                }
                
                override fun onDisconnected(camera: CameraDevice) {
                    log("Camera disconnected")
                    camera.close()
                    cameraDevice = null
                }
                
                override fun onError(camera: CameraDevice, error: Int) {
                    val errorStr = when (error) {
                        ERROR_CAMERA_IN_USE -> "CAMERA_IN_USE"
                        ERROR_MAX_CAMERAS_IN_USE -> "MAX_CAMERAS_IN_USE"
                        ERROR_CAMERA_DISABLED -> "CAMERA_DISABLED"
                        ERROR_CAMERA_DEVICE -> "CAMERA_DEVICE"
                        ERROR_CAMERA_SERVICE -> "CAMERA_SERVICE"
                        else -> "UNKNOWN($error)"
                    }
                    log("✗ Camera error: $errorStr")
                    camera.close()
                    cameraDevice = null
                    finishTest(false, "Camera error: $errorStr")
                }
            }, backgroundHandler)
            
        } catch (e: CameraAccessException) {
            log("✗ Failed to open camera: ${e.message}")
            finishTest(false, "CameraAccessException: ${e.message}")
        }
    }
    
    private fun createCaptureSession() {
        try {
            updateStatus("Configuring stream...")
            log("Creating ImageReader ${WIDTH}x${HEIGHT} RGBA")
            
            // Create ImageReader for RGBA output
            imageReader = ImageReader.newInstance(
                WIDTH, HEIGHT,
                ImageFormat.FLEX_RGBA_8888,  // or ImageFormat.YUV_420_888
                4  // maxImages
            ).apply {
                setOnImageAvailableListener({ reader ->
                    val image = reader.acquireLatestImage()
                    if (image != null) {
                        processFrame(image)
                        image.close()
                    }
                }, backgroundHandler)
            }
            
            val surface = imageReader!!.surface
            
            log("Creating capture session...")
            cameraDevice?.createCaptureSession(
                listOf(surface),
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(session: CameraCaptureSession) {
                        log("✓ Capture session configured")
                        captureSession = session
                        startCapturing(surface)
                    }
                    
                    override fun onConfigureFailed(session: CameraCaptureSession) {
                        log("✗ Failed to configure capture session")
                        finishTest(false, "Session configuration failed")
                    }
                },
                backgroundHandler
            )
            
        } catch (e: Exception) {
            log("✗ Error creating session: ${e.message}")
            finishTest(false, "Exception: ${e.message}")
        }
    }
    
    private fun startCapturing(surface: Surface) {
        try {
            updateStatus("Capturing frames...")
            log("Starting capture...")
            
            val captureRequest = cameraDevice?.createCaptureRequest(
                CameraDevice.TEMPLATE_PREVIEW
            )?.apply {
                addTarget(surface)
            }?.build()
            
            testStartTime = System.currentTimeMillis()
            
            captureSession?.setRepeatingRequest(
                captureRequest!!,
                object : CameraCaptureSession.CaptureCallback() {
                    override fun onCaptureCompleted(
                        session: CameraCaptureSession,
                        request: CaptureRequest,
                        result: TotalCaptureResult
                    ) {
                        // Frame captured successfully
                    }
                    
                    override fun onCaptureFailed(
                        session: CameraCaptureSession,
                        request: CaptureRequest,
                        failure: CaptureFailure
                    ) {
                        log("Capture failed: ${failure.reason}")
                    }
                },
                backgroundHandler
            )
            
            // Schedule test end
            backgroundHandler?.postDelayed({
                stopCapturing()
            }, TEST_DURATION_MS)
            
        } catch (e: Exception) {
            log("✗ Error starting capture: ${e.message}")
            finishTest(false, "Capture exception: ${e.message}")
        }
    }
    
    private fun processFrame(image: android.media.Image) {
        val now = System.currentTimeMillis()
        val count = frameCount.incrementAndGet()
        
        if (firstFrameTime.get() == 0L) {
            firstFrameTime.set(now)
            log("✓ First frame received!")
        }
        lastFrameTime.set(now)
        
        // Analyze frame content (first frame only for performance)
        if (count == 1) {
            analyzeFrameContent(image)
        }
        
        // Log progress every 10 frames
        if (count % 10 == 0) {
            val elapsed = now - testStartTime
            val fps = count * 1000.0 / elapsed
            log("  Frame $count (${String.format("%.1f", fps)} fps)")
        }
    }
    
    private fun analyzeFrameContent(image: android.media.Image) {
        try {
            val planes = image.planes
            log("Frame analysis:")
            log("  Format: ${image.format}")
            log("  Size: ${image.width}x${image.height}")
            log("  Planes: ${planes.size}")
            
            if (planes.isNotEmpty()) {
                val buffer = planes[0].buffer
                val rowStride = planes[0].rowStride
                val pixelStride = planes[0].pixelStride
                log("  Row stride: $rowStride")
                log("  Pixel stride: $pixelStride")
                
                // Sample a few pixels
                if (buffer.remaining() >= 4) {
                    val r = buffer.get(0).toInt() and 0xFF
                    val g = buffer.get(1).toInt() and 0xFF
                    val b = buffer.get(2).toInt() and 0xFF
                    val a = buffer.get(3).toInt() and 0xFF
                    log("  Pixel[0,0]: RGBA($r, $g, $b, $a)")
                    
                    // Check if it's not all zeros (black)
                    if (r == 0 && g == 0 && b == 0) {
                        log("  ⚠ Warning: Pixel is black (may be uninitialized)")
                    } else {
                        log("  ✓ Frame has content!")
                    }
                }
            }
        } catch (e: Exception) {
            log("Frame analysis error: ${e.message}")
        }
    }
    
    private fun stopCapturing() {
        log("")
        log("Stopping capture...")
        
        captureSession?.stopRepeating()
        captureSession?.close()
        captureSession = null
        
        cameraDevice?.close()
        cameraDevice = null
        
        imageReader?.close()
        imageReader = null
        
        // Calculate results
        val totalFrames = frameCount.get()
        val duration = lastFrameTime.get() - firstFrameTime.get()
        val fps = if (duration > 0) totalFrames * 1000.0 / duration else 0.0
        
        val success = totalFrames > 0
        
        log("")
        log("=== Test Results ===")
        log("Total frames: $totalFrames")
        log("Duration: ${duration}ms")
        log("Average FPS: ${String.format("%.2f", fps)}")
        log("Status: ${if (success) "PASS ✓" else "FAIL ✗"}")
        
        stopBackgroundThread()
        finishTest(success, if (success) "Captured $totalFrames frames at ${String.format("%.1f", fps)} fps" else "No frames captured")
    }
    
    private fun finishTest(success: Boolean, message: String) {
        runOnUiThread {
            startButton.isEnabled = true
            updateStatus(if (success) "✓ TEST PASSED" else "✗ TEST FAILED")
            resultsText.text = results.toString()
        }
    }
    
    private fun log(message: String) {
        Log.i(TAG, message)
        results.append(message).append("\n")
        runOnUiThread {
            resultsText.text = results.toString()
        }
    }
    
    private fun updateStatus(status: String) {
        runOnUiThread {
            statusText.text = status
        }
    }
    
    override fun onDestroy() {
        super.onDestroy()
        captureSession?.close()
        cameraDevice?.close()
        imageReader?.close()
        stopBackgroundThread()
    }
}
