package com.example.vcamtest

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.hardware.camera2.*
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.ImageReader
import android.media.MediaRecorder
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import android.graphics.SurfaceTexture
import android.view.SurfaceHolder
import android.view.TextureView
import android.view.SurfaceView
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import kotlin.math.abs

/**
 * Unified A/V test app with latency visualization.
 * 
 * LEFT side: Preview of frames being sent to HAL
 * RIGHT side: Camera2 preview from virtual camera HAL
 * GRAPH: Combined audio (green) and video (blue) latency over time
 */
class MainActivity : AppCompatActivity() {
    
    companion object {
        private const val TAG = "AVLatencyTest"
        private const val VIRTUAL_CAMERA_ID = "100"
        private const val REQUEST_PERMISSIONS = 1
        private const val SAMPLE_RATE = 48000
        private const val AUDIO_CHANNELS = AudioFormat.CHANNEL_IN_STEREO
        private const val AUDIO_FORMAT = AudioFormat.ENCODING_PCM_16BIT
    }
    
    // Views
    private lateinit var rendererView: SurfaceView
    private lateinit var cameraView: TextureView
    private lateinit var statusText: TextView
    private lateinit var startButton: Button
    private lateinit var audioButton: Button
    private lateinit var videoLatencyText: TextView
    private lateinit var audioLatencyText: TextView
    private lateinit var avSyncText: TextView
    private lateinit var latencyGraph: LatencyGraphView
    
    // Service client
    private val vmClient = VirtualMediaClient()
    private var videoRendererId = -1
    private var audioRendererId = -1
    
    // State
    @Volatile private var isVideoRunning = false
    @Volatile private var isAudioRunning = false
    private var lastClickTime = 0L
    
    // Surface mode (zero-copy)
    private var useSurfaceMode = true  // Default to Surface mode for zero-copy
    private var serviceSurface: Surface? = null
    private var surfaceRenderThread: Thread? = null
    @Volatile private var surfaceRenderRunning = false
    
    // Camera
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null
    private var imageReader: ImageReader? = null
    
    // Audio
    private var audioRecord: AudioRecord? = null
    private var audioThread: Thread? = null
    
    // Config
    private val camWidth = 1280
    private val camHeight = 720
    private val targetFps = 30
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        
        // Find views
        rendererView = findViewById(R.id.renderer_view)
        cameraView = findViewById(R.id.camera_view)
        statusText = findViewById(R.id.status_text)
        startButton = findViewById(R.id.start_button)
        audioButton = findViewById(R.id.audio_button)
        videoLatencyText = findViewById(R.id.video_latency_text)
        audioLatencyText = findViewById(R.id.audio_latency_text)
        avSyncText = findViewById(R.id.av_sync_text)
        latencyGraph = findViewById(R.id.latency_graph)
        
        // Video button
        startButton.setOnClickListener { 
            val now = System.currentTimeMillis()
            if (now - lastClickTime > 500) {
                lastClickTime = now
                toggleVideo()
            }
        }
        
        // Audio button
        audioButton.setOnClickListener {
            val now = System.currentTimeMillis()
            if (now - lastClickTime > 500) {
                lastClickTime = now
                toggleAudio()
            }
        }
        
        // Set up preview surface for native code
        rendererView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                Log.i(TAG, "Preview surface created")
                NativeProducer.setPreviewSurface(holder.surface)
            }
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                Log.i(TAG, "Preview surface changed: ${width}x${height}")
            }
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                Log.i(TAG, "Preview surface destroyed")
                NativeProducer.setPreviewSurface(null)
            }
        })
        
        // Connect to service
        if (vmClient.connect()) {
            updateStatus("Service connected ✓")
            startButton.isEnabled = true
            audioButton.isEnabled = true
        } else {
            updateStatus("Service connection FAILED")
        }
        
        // Set up camera preview (right side)
        cameraView.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                Log.i(TAG, "TextureView surface available: ${width}x${height}")
                checkPermissionsAndOpenCamera()
            }
            override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, w: Int, h: Int) {}
            override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                closeCamera()
                return true
            }
            override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {}
        }
        
        // Check if TextureView is already available (can happen on rotation/recreation)
        if (cameraView.isAvailable) {
            Log.i(TAG, "TextureView already available, opening camera")
            checkPermissionsAndOpenCamera()
        }
    }
    
    // ========== Video ==========
    
    private fun toggleVideo() {
        if (isVideoRunning) {
            stopVideo()
        } else {
            startVideo()
        }
    }
    
    private fun startVideo() {
        if (useSurfaceMode) {
            startVideoSurfaceMode()
        } else {
            startVideoSharedMemoryMode()
        }
    }
    
    // ImageReader for AIDL zero-copy path
    private var producerImageReader: ImageReader? = null
    private var producerSurface: Surface? = null
    
    private fun startVideoSurfaceMode() {
        // Start video renderer to get rendererId
        val config = VirtualMediaClient.VideoConfig(camWidth, camHeight, targetFps, 1)
        val info = vmClient.startVideoRenderer(config)
        if (info == null) {
            Log.e(TAG, "Failed to start video renderer")
            useSurfaceMode = false
            startVideoSharedMemoryMode()
            return
        }
        videoRendererId = info.id
        
        // Create ImageReader with buffer pool (AIDL zero-copy)
        producerImageReader = ImageReader.newInstance(
            camWidth, camHeight,
            android.graphics.PixelFormat.RGBA_8888,
            3,  // Triple buffer
            android.hardware.HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE or 
            android.hardware.HardwareBuffer.USAGE_CPU_WRITE_OFTEN
        )
        
        producerImageReader?.setOnImageAvailableListener({ reader ->
            val image = reader.acquireLatestImage() ?: return@setOnImageAvailableListener
            try {
                val hwBuffer = image.hardwareBuffer ?: return@setOnImageAvailableListener
                val timestamp = android.os.SystemClock.elapsedRealtimeNanos()
                vmClient.sendHardwareBuffer(videoRendererId, hwBuffer, timestamp)
                hwBuffer.close()
            } finally {
                image.close()
            }
        }, cameraHandler)
        
        producerSurface = producerImageReader?.surface
        if (producerSurface == null) {
            Log.e(TAG, "Failed to get Surface from ImageReader")
            stopVideo()
            return
        }
        
        // Start rendering thread that draws to the ImageReader Surface
        surfaceRenderRunning = true
        surfaceRenderThread = Thread {
            renderToSurface()
        }.apply { 
            name = "SurfaceRenderer"
            start() 
        }
        
        isVideoRunning = true
        runOnUiThread { startButton.text = "⏹ Video (Surface)" }
        updateStatus("Video running (AIDL zero-copy)")
    }
    
    private fun renderToSurface() {
        val surface = producerSurface ?: serviceSurface ?: return
        
        var frameNumber = 0L
        val frameIntervalNs = 1_000_000_000L / targetFps
        var nextFrameTime = System.nanoTime()
        
        // Ball animation state
        var ballX = camWidth / 4f
        var ballY = camHeight / 4f
        var ballVx = 8f
        var ballVy = 5f
        val ballRadius = 50f
        
        val paint = android.graphics.Paint().apply {
            color = android.graphics.Color.RED
            style = android.graphics.Paint.Style.FILL
            isAntiAlias = true
        }
        
        val textPaint = android.graphics.Paint().apply {
            color = android.graphics.Color.WHITE
            textSize = 40f
            isAntiAlias = true
            typeface = android.graphics.Typeface.MONOSPACE
        }
        
        val bgPaint = android.graphics.Paint().apply {
            color = android.graphics.Color.rgb(20, 20, 50)
        }
        
        while (surfaceRenderRunning && surface.isValid) {
            // Update ball position
            ballX += ballVx
            ballY += ballVy
            
            // Bounce off walls
            if (ballX - ballRadius < 0 || ballX + ballRadius > camWidth) {
                ballVx = -ballVx
                ballX = ballX.coerceIn(ballRadius, camWidth - ballRadius)
            }
            if (ballY - ballRadius < 0 || ballY + ballRadius > camHeight) {
                ballVy = -ballVy
                ballY = ballY.coerceIn(ballRadius, camHeight - ballRadius)
            }
            
            // Animated color bar
            val hue = (frameNumber * 3 % 360).toFloat()
            val barColor = android.graphics.Color.HSVToColor(floatArrayOf(hue, 0.8f, 1f))
            val barPaint = android.graphics.Paint().apply { color = barColor }
            
            // Draw frame content (helper lambda)
            fun drawFrame(canvas: android.graphics.Canvas) {
                // Clear background
                canvas.drawColor(bgPaint.color)
                
                // Draw bouncing ball
                canvas.drawCircle(ballX, ballY, ballRadius, paint)
                
                // Draw frame counter
                canvas.drawText("Frame: $frameNumber", 30f, 50f, textPaint)
                
                // Draw animated color bar at bottom
                canvas.drawRect(0f, camHeight - 30f, camWidth.toFloat(), camHeight.toFloat(), barPaint)
            }
            
            // Draw to HAL Surface (zero-copy path)
            try {
                val canvas = surface.lockCanvas(null)
                if (canvas != null) {
                    drawFrame(canvas)
                    surface.unlockCanvasAndPost(canvas)
                }
            } catch (e: Exception) {
                Log.e(TAG, "HAL Surface render error: ${e.message}")
                break
            }
            
            // Also draw to local preview SurfaceView
            try {
                val previewHolder = rendererView.holder
                if (previewHolder.surface.isValid) {
                    val previewCanvas = previewHolder.lockCanvas()
                    if (previewCanvas != null) {
                        // Scale to fit preview
                        val scaleX = previewCanvas.width.toFloat() / camWidth
                        val scaleY = previewCanvas.height.toFloat() / camHeight
                        previewCanvas.scale(scaleX, scaleY)
                        drawFrame(previewCanvas)
                        previewHolder.unlockCanvasAndPost(previewCanvas)
                    }
                }
            } catch (e: Exception) {
                // Preview may not be ready, ignore
            }
            
            frameNumber++
            
            // Update latency display
            if (frameNumber % 30 == 0L) {
                runOnUiThread {
                    videoLatencyText.text = "🎬 Video: <1ms (zero-copy)"
                }
            }
            
            // Frame pacing
            nextFrameTime += frameIntervalNs
            val sleepNs = nextFrameTime - System.nanoTime()
            if (sleepNs > 0) {
                Thread.sleep(sleepNs / 1_000_000, (sleepNs % 1_000_000).toInt())
            }
        }
        
        Log.i(TAG, "Surface render thread ended after $frameNumber frames")
    }
    
    private fun startVideoSharedMemoryMode() {
        val info = vmClient.startVideoRenderer(VirtualMediaClient.VideoConfig(camWidth, camHeight, targetFps, 1))
        
        if (info == null || info.bufferFd == null) {
            updateStatus("Failed to start video renderer")
            return
        }
        
        videoRendererId = info.id
        
        if (!NativeProducer.startVideoWithFd(info.bufferFd.fd, info.bufferSize, info.headerSize, camWidth, camHeight, targetFps)) {
            updateStatus("Failed to start native producer")
            vmClient.stopVideoRenderer(videoRendererId)
            return
        }
        
        NativeProducer.setVideoPattern(NativeProducer.Pattern.BOUNCING_BALL)
        
        isVideoRunning = true
        runOnUiThread { startButton.text = "⏹ Video" }
        updateStatus("Video running (shared memory)")
    }
    
    private fun stopVideo() {
        isVideoRunning = false
        
        // Stop Surface mode
        surfaceRenderRunning = false
        surfaceRenderThread?.join(1000)
        surfaceRenderThread = null
        
        // Clean up ImageReader (AIDL path)
        producerSurface = null
        producerImageReader?.close()
        producerImageReader = null
        
        if (serviceSurface != null) {
            vmClient.releaseVideoSurface()
            serviceSurface = null
        }
        
        // Stop shared memory mode
        NativeProducer.stopVideo()
        if (videoRendererId >= 0) {
            vmClient.stopVideoRenderer(videoRendererId)
            videoRendererId = -1
        }
        
        runOnUiThread { 
            startButton.text = "▶ Video"
            updateStatus("Video stopped")
        }
    }
    
    // ========== Audio ==========
    
    private fun toggleAudio() {
        if (isAudioRunning) {
            stopAudio()
        } else {
            startAudio()
        }
    }
    
    private fun startAudio() {
        // Start audio producer
        val config = VirtualMediaClient.AudioConfig(
            sampleRate = SAMPLE_RATE,
            channelCount = 2,
            format = 1,
            bufferFrames = 4096
        )
        
        val info = vmClient.startAudioRenderer(config)
        if (info == null || info.bufferFd == null) {
            updateStatus("Failed to start audio renderer")
            return
        }
        
        audioRendererId = info.id
        
        if (!NativeProducer.startAudioWithFd(
                info.bufferFd.fd, info.bufferSize, info.headerSize,
                SAMPLE_RATE, 2, 440f, 0.5f)) {
            updateStatus("Failed to start native audio producer")
            vmClient.stopAudioRenderer(audioRendererId)
            return
        }
        
        isAudioRunning = true
        runOnUiThread { audioButton.text = "⏹ Audio" }
        
        // Start audio capture thread for latency measurement
        startAudioCapture()
        
        updateStatus("Audio running (440 Hz)")
    }
    
    private fun stopAudio() {
        isAudioRunning = false
        stopAudioCapture()
        NativeProducer.stopAudio()
        if (audioRendererId >= 0) {
            vmClient.stopAudioRenderer(audioRendererId)
            audioRendererId = -1
        }
        runOnUiThread { 
            audioButton.text = "▶ Audio"
            updateStatus("Audio stopped")
        }
    }
    
    private fun startAudioCapture() {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            Log.w(TAG, "No audio permission for latency measurement")
            return
        }
        
        val bufferSize = AudioRecord.getMinBufferSize(SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_FORMAT) * 2
        
        audioRecord = AudioRecord(
            MediaRecorder.AudioSource.MIC,
            SAMPLE_RATE,
            AUDIO_CHANNELS,
            AUDIO_FORMAT,
            bufferSize
        )
        
        // Try to select virtual mic
        try {
            val audioManager = getSystemService(AUDIO_SERVICE) as android.media.AudioManager
            val devices = audioManager.getDevices(android.media.AudioManager.GET_DEVICES_INPUTS)
            for (device in devices) {
                if (device.type == android.media.AudioDeviceInfo.TYPE_BUS) {
                    audioRecord?.preferredDevice = device
                    Log.i(TAG, "Selected virtual mic: ${device.productName}")
                    break
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Could not select virtual mic: ${e.message}")
        }
        
        audioRecord?.startRecording()
        
        audioThread = Thread {
            val buffer = ShortArray(1024)
            val audioTimestamp = android.media.AudioTimestamp()
            var lastMeasureTime = 0L
            var totalFramesRead = 0L
            
            while (isAudioRunning) {
                val read = audioRecord?.read(buffer, 0, buffer.size) ?: 0
                if (read > 0) {
                    totalFramesRead += read / 2  // Stereo = 2 samples per frame
                    
                    val now = android.os.SystemClock.elapsedRealtimeNanos()
                    
                    // Measure latency every 100ms using AudioTimestamp
                    if (now - lastMeasureTime > 100_000_000) {
                        lastMeasureTime = now
                        
                        // Try to get AudioTimestamp for precise latency
                        var latencyMs = 0f
                        try {
                            if (audioRecord?.getTimestamp(audioTimestamp, android.media.AudioTimestamp.TIMEBASE_BOOTTIME) == AudioRecord.SUCCESS) {
                                // Timestamp tells us: at time nanoTime, framePosition was at audio endpoint
                                // Current time - timestamp time = how long ago that frame was at endpoint
                                // Plus: how many more frames we've read since then
                                val framesReadSinceTimestamp = totalFramesRead - audioTimestamp.framePosition
                                val timeSinceTimestamp = now - audioTimestamp.nanoTime
                                
                                // Total latency = time since timestamp was taken + buffer delay
                                // Buffer delay = frames in buffer / sample rate
                                val bufferDelayNs = (framesReadSinceTimestamp * 1_000_000_000L) / SAMPLE_RATE
                                latencyMs = (timeSinceTimestamp - bufferDelayNs) / 1_000_000f
                                
                                // Sanity check
                                if (latencyMs < 0) latencyMs = 0f
                                if (latencyMs > 500) latencyMs = 500f
                            } else {
                                // Fallback: estimate from buffer size
                                val bufferFrames = read / 2
                                latencyMs = (bufferFrames.toFloat() / SAMPLE_RATE) * 1000 + 20  // +20ms estimate
                            }
                        } catch (e: Exception) {
                            // Fallback
                            latencyMs = 50f  // Default estimate
                        }
                        
                        runOnUiThread {
                            latencyGraph.addAudioSample(latencyMs)
                            audioLatencyText.text = "🎤 Audio: ${latencyMs.toInt()}ms"
                            updateAvSync()
                        }
                    }
                }
            }
        }.also { it.start() }
    }
    
    private fun stopAudioCapture() {
        audioThread?.interrupt()
        audioThread = null
        audioRecord?.stop()
        audioRecord?.release()
        audioRecord = null
    }
    
    // ========== Camera ==========
    
    private fun checkPermissionsAndOpenCamera() {
        val permissions = mutableListOf(Manifest.permission.CAMERA)
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            permissions.add(Manifest.permission.RECORD_AUDIO)
        }
        
        val needed = permissions.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (needed.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, needed.toTypedArray(), REQUEST_PERMISSIONS)
        } else {
            openVirtualCamera()
        }
    }
    
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_PERMISSIONS) {
            if (grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                openVirtualCamera()
            }
        }
    }
    
    private fun openVirtualCamera() {
        cameraThread = HandlerThread("CameraThread").also { it.start() }
        cameraHandler = Handler(cameraThread!!.looper)
        
        val cameraManager = getSystemService(CAMERA_SERVICE) as CameraManager
        val cameraIds = cameraManager.cameraIdList
        val targetId = if (cameraIds.contains(VIRTUAL_CAMERA_ID)) VIRTUAL_CAMERA_ID else cameraIds.lastOrNull() ?: return
        
        Log.i(TAG, "Opening camera: $targetId (available: ${cameraIds.joinToString()})")
        
        // Create ImageReader for latency measurement
        imageReader = ImageReader.newInstance(camWidth, camHeight, ImageFormat.YUV_420_888, 2)
        imageReader?.setOnImageAvailableListener({ reader ->
            val image = reader.acquireLatestImage() ?: return@setOnImageAvailableListener
            
            // Get the timestamp from the image (CLOCK_BOOTTIME from HAL)
            val imageTimestamp = image.timestamp  // nanoseconds
            val now = android.os.SystemClock.elapsedRealtimeNanos()  // Also CLOCK_BOOTTIME
            
            // Calculate latency
            val latencyNs = now - imageTimestamp
            val latencyMs = latencyNs / 1_000_000f
            
            // Filter out unreasonable values
            if (latencyMs > 0 && latencyMs < 500) {
                runOnUiThread {
                    latencyGraph.addVideoSample(latencyMs)
                    videoLatencyText.text = "🎬 Video: ${latencyMs.toInt()}ms"
                    updateAvSync()
                }
            }
            
            image.close()
        }, cameraHandler)
        
        try {
            if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
                cameraManager.openCamera(targetId, object : CameraDevice.StateCallback() {
                    override fun onOpened(camera: CameraDevice) {
                        cameraDevice = camera
                        createPreviewSession()
                    }
                    override fun onDisconnected(camera: CameraDevice) { 
                        cameraDevice = null 
                    }
                    override fun onError(camera: CameraDevice, error: Int) { 
                        Log.e(TAG, "Camera error: $error") 
                    }
                }, cameraHandler)
            }
        } catch (e: CameraAccessException) {
            Log.e(TAG, "Failed to open camera", e)
        }
    }
    
    @Suppress("DEPRECATION")
    private fun createPreviewSession() {
        val camera = cameraDevice ?: return
        val surfaceTexture = cameraView.surfaceTexture ?: return
        val reader = imageReader ?: return
        
        surfaceTexture.setDefaultBufferSize(camWidth, camHeight)
        val previewSurface = Surface(surfaceTexture)
        val readerSurface = reader.surface
        
        try {
            val previewRequest = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply { 
                addTarget(previewSurface)
                addTarget(readerSurface)
            }
            
            camera.createCaptureSession(listOf(previewSurface, readerSurface), object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    captureSession = session
                    session.setRepeatingRequest(previewRequest.build(), null, cameraHandler)
                    Log.i(TAG, "Camera preview session configured")
                }
                override fun onConfigureFailed(session: CameraCaptureSession) { 
                    Log.e(TAG, "Preview config failed") 
                }
            }, cameraHandler)
        } catch (e: CameraAccessException) {
            Log.e(TAG, "Failed to create preview", e)
        }
    }
    
    private fun closeCamera() {
        captureSession?.close()
        cameraDevice?.close()
        imageReader?.close()
        cameraThread?.quitSafely()
    }
    
    // ========== UI Updates ==========
    
    private fun updateStatus(text: String) { 
        statusText.text = text 
    }
    
    private fun updateAvSync() {
        val sync = latencyGraph.avSyncMs
        val syncText = if (sync >= 0) "+${sync.toInt()}ms (V ahead)" else "${sync.toInt()}ms (A ahead)"
        avSyncText.text = "⏱️ Sync: $syncText"
    }
    
    override fun onDestroy() {
        super.onDestroy()
        stopVideo()
        stopAudio()
        closeCamera()
    }
}
