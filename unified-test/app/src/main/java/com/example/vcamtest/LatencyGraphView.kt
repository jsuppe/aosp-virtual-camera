package com.example.vcamtest

import android.content.Context
import android.graphics.*
import android.util.AttributeSet
import android.view.View
import kotlin.math.max
import kotlin.math.min

/**
 * Combined A/V latency visualization graph.
 * Shows video (blue) and audio (green) latency as overlapping sparklines.
 */
class LatencyGraphView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    companion object {
        private const val MAX_SAMPLES = 150  // ~5 seconds at 30fps
        private const val GOOD_LATENCY_MS = 30f
        private const val WARN_LATENCY_MS = 50f
    }

    // Data storage
    private val videoSamples = ArrayDeque<Float>(MAX_SAMPLES)
    private val audioSamples = ArrayDeque<Float>(MAX_SAMPLES)
    
    // Stats
    var videoLatencyMs: Float = 0f
        private set
    var audioLatencyMs: Float = 0f
        private set
    var videoAvgMs: Float = 0f
        private set
    var audioAvgMs: Float = 0f
        private set
    var avSyncMs: Float = 0f  // positive = video ahead
        private set

    // Paints
    private val videoPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#2196F3")  // Blue
        strokeWidth = 3f
        style = Paint.Style.STROKE
    }
    
    private val audioPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#4CAF50")  // Green
        strokeWidth = 3f
        style = Paint.Style.STROKE
    }
    
    private val gridPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#333333")
        strokeWidth = 1f
        style = Paint.Style.STROKE
    }
    
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#888888")
        textSize = 24f
    }
    
    private val warnPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#FF9800")
        strokeWidth = 1f
        style = Paint.Style.STROKE
        pathEffect = DashPathEffect(floatArrayOf(10f, 10f), 0f)
    }
    
    private val goodPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#4CAF50")
        strokeWidth = 1f
        style = Paint.Style.STROKE
        pathEffect = DashPathEffect(floatArrayOf(5f, 5f), 0f)
    }

    private val videoPath = Path()
    private val audioPath = Path()

    fun addVideoSample(latencyMs: Float) {
        synchronized(videoSamples) {
            if (videoSamples.size >= MAX_SAMPLES) {
                videoSamples.removeFirst()
            }
            videoSamples.addLast(latencyMs)
            videoLatencyMs = latencyMs
            videoAvgMs = videoSamples.average().toFloat()
        }
        updateAvSync()
        postInvalidate()
    }

    fun addAudioSample(latencyMs: Float) {
        synchronized(audioSamples) {
            if (audioSamples.size >= MAX_SAMPLES) {
                audioSamples.removeFirst()
            }
            audioSamples.addLast(latencyMs)
            audioLatencyMs = latencyMs
            audioAvgMs = audioSamples.average().toFloat()
        }
        updateAvSync()
        postInvalidate()
    }

    private fun updateAvSync() {
        avSyncMs = videoAvgMs - audioAvgMs
    }

    fun clear() {
        synchronized(videoSamples) { videoSamples.clear() }
        synchronized(audioSamples) { audioSamples.clear() }
        videoLatencyMs = 0f
        audioLatencyMs = 0f
        videoAvgMs = 0f
        audioAvgMs = 0f
        avSyncMs = 0f
        postInvalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        
        val w = width.toFloat()
        val h = height.toFloat()
        val padding = 40f
        val graphLeft = padding
        val graphRight = w - padding
        val graphTop = padding
        val graphBottom = h - padding
        val graphWidth = graphRight - graphLeft
        val graphHeight = graphBottom - graphTop

        // Find max value for scaling
        val maxLatency = max(
            max(videoSamples.maxOrNull() ?: 50f, audioSamples.maxOrNull() ?: 50f),
            WARN_LATENCY_MS * 1.5f
        )

        // Draw grid lines
        drawGrid(canvas, graphLeft, graphTop, graphRight, graphBottom, maxLatency)

        // Draw threshold lines
        val goodY = graphBottom - (GOOD_LATENCY_MS / maxLatency) * graphHeight
        val warnY = graphBottom - (WARN_LATENCY_MS / maxLatency) * graphHeight
        canvas.drawLine(graphLeft, goodY, graphRight, goodY, goodPaint)
        canvas.drawLine(graphLeft, warnY, graphRight, warnY, warnPaint)

        // Draw video samples (blue)
        synchronized(videoSamples) {
            if (videoSamples.isNotEmpty()) {
                drawSamples(canvas, videoSamples.toList(), videoPath, videoPaint,
                    graphLeft, graphTop, graphWidth, graphHeight, maxLatency)
            }
        }

        // Draw audio samples (green)
        synchronized(audioSamples) {
            if (audioSamples.isNotEmpty()) {
                drawSamples(canvas, audioSamples.toList(), audioPath, audioPaint,
                    graphLeft, graphTop, graphWidth, graphHeight, maxLatency)
            }
        }

        // Draw legend
        drawLegend(canvas, graphLeft, graphTop)
    }

    private fun drawGrid(canvas: Canvas, left: Float, top: Float, right: Float, bottom: Float, maxLatency: Float) {
        // Horizontal grid lines (latency levels)
        val steps = 4
        for (i in 0..steps) {
            val y = top + (bottom - top) * i / steps
            canvas.drawLine(left, y, right, y, gridPaint)
            
            val latencyValue = maxLatency * (steps - i) / steps
            canvas.drawText("${latencyValue.toInt()}ms", 4f, y + 8f, textPaint)
        }
    }

    private fun drawSamples(
        canvas: Canvas,
        samples: List<Float>,
        path: Path,
        paint: Paint,
        graphLeft: Float,
        graphTop: Float,
        graphWidth: Float,
        graphHeight: Float,
        maxLatency: Float
    ) {
        if (samples.isEmpty()) return
        
        path.reset()
        val stepX = graphWidth / MAX_SAMPLES
        val startX = graphLeft + (MAX_SAMPLES - samples.size) * stepX
        
        samples.forEachIndexed { index, latency ->
            val x = startX + index * stepX
            val y = graphTop + graphHeight - (latency / maxLatency) * graphHeight
            val clampedY = min(max(y, graphTop), graphTop + graphHeight)
            
            if (index == 0) {
                path.moveTo(x, clampedY)
            } else {
                path.lineTo(x, clampedY)
            }
        }
        
        canvas.drawPath(path, paint)
    }

    private fun drawLegend(canvas: Canvas, left: Float, top: Float) {
        val legendY = top - 15f
        
        // Video legend
        videoPaint.style = Paint.Style.FILL
        canvas.drawCircle(left, legendY, 6f, videoPaint)
        videoPaint.style = Paint.Style.STROKE
        canvas.drawText("Video", left + 12f, legendY + 5f, textPaint)
        
        // Audio legend
        audioPaint.style = Paint.Style.FILL
        canvas.drawCircle(left + 80f, legendY, 6f, audioPaint)
        audioPaint.style = Paint.Style.STROKE
        canvas.drawText("Audio", left + 92f, legendY + 5f, textPaint)
    }
}
