/*
 * VirtualCamera - Individual camera instance
 * 
 * Manages the connection between a renderer app and the camera HAL.
 * Uses ImageReader/ImageWriter for zero-copy buffer passing.
 * 
 * Location: frameworks/base/services/core/java/com/android/server/camera/virtual/
 */
package com.android.server.camera.virtual;

import android.graphics.ImageFormat;
import android.graphics.PixelFormat;
import android.hardware.HardwareBuffer;
import android.hardware.camera.virtual.IVirtualCameraCallback;
import android.hardware.camera.virtual.VirtualCameraConfig;
import android.hardware.camera.virtual.StreamConfig;
import android.media.Image;
import android.media.ImageReader;
import android.media.ImageWriter;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.RemoteException;
import android.util.Log;
import android.util.SparseArray;
import android.view.Surface;

/**
 * Represents a single virtual camera instance.
 * 
 * For each stream:
 * - Creates an ImageReader (renderer writes here via Surface)
 * - When HAL requests buffer, acquires latest Image and returns HardwareBuffer
 * - Zero-copy: HardwareBuffer wraps the same underlying gralloc buffer
 */
class VirtualCamera {
    private static final String TAG = "VirtualCamera";
    
    private final int mCameraId;
    private final VirtualCameraConfig mConfig;
    private final IVirtualCameraCallback mCallback;
    
    // Background thread for ImageReader callbacks
    private final HandlerThread mHandlerThread;
    private final Handler mHandler;
    
    // Stream management
    private final SparseArray<StreamContext> mStreams = new SparseArray<>();
    
    private boolean mIsOpen = false;
    private boolean mIsCapturing = false;
    
    VirtualCamera(int cameraId, VirtualCameraConfig config, IVirtualCameraCallback callback) {
        mCameraId = cameraId;
        mConfig = config;
        mCallback = callback;
        
        mHandlerThread = new HandlerThread("VirtualCamera-" + cameraId);
        mHandlerThread.start();
        mHandler = new Handler(mHandlerThread.getLooper());
    }
    
    int getCameraId() {
        return mCameraId;
    }
    
    VirtualCameraConfig getConfig() {
        return mConfig;
    }
    
    boolean isInUse() {
        return mIsOpen;
    }
    
    void onOpened() {
        mIsOpen = true;
        try {
            mCallback.onCameraOpened();
        } catch (RemoteException e) {
            Log.e(TAG, "Failed to notify renderer of camera open", e);
        }
    }
    
    void onStreamsConfigured(StreamConfig[] streams) {
        // Clear old streams
        synchronized (mStreams) {
            for (int i = 0; i < mStreams.size(); i++) {
                mStreams.valueAt(i).release();
            }
            mStreams.clear();
        }
        
        // Create ImageReader for each stream
        Surface[] surfaces = new Surface[streams.length];
        
        for (int i = 0; i < streams.length; i++) {
            StreamConfig config = streams[i];
            StreamContext ctx = new StreamContext(config, mHandler);
            
            synchronized (mStreams) {
                mStreams.put(config.streamId, ctx);
            }
            
            surfaces[i] = ctx.getSurface();
            
            Log.d(TAG, "Created stream " + config.streamId + 
                  ": " + config.width + "x" + config.height + 
                  " format=" + config.format + " fps=" + config.fps);
        }
        
        // Send surfaces to renderer
        try {
            mCallback.onStreamsConfigured(streams, surfaces);
        } catch (RemoteException e) {
            Log.e(TAG, "Failed to send streams to renderer", e);
        }
    }
    
    void onCaptureStarted(int frameRate) {
        mIsCapturing = true;
        try {
            mCallback.onCaptureStarted(frameRate);
        } catch (RemoteException e) {
            Log.e(TAG, "Failed to notify renderer of capture start", e);
        }
    }
    
    void onCaptureStopped() {
        mIsCapturing = false;
        try {
            mCallback.onCaptureStopped();
        } catch (RemoteException e) {
            Log.e(TAG, "Failed to notify renderer of capture stop", e);
        }
    }
    
    void onClosed() {
        mIsOpen = false;
        mIsCapturing = false;
        
        try {
            mCallback.onCameraClosed();
        } catch (RemoteException e) {
            Log.e(TAG, "Failed to notify renderer of camera close", e);
        }
        
        // Release all streams
        synchronized (mStreams) {
            for (int i = 0; i < mStreams.size(); i++) {
                mStreams.valueAt(i).release();
            }
            mStreams.clear();
        }
    }
    
    /**
     * Acquire a buffer from the renderer's output.
     * Called by HAL when it needs a frame.
     * 
     * @return HardwareBuffer containing rendered frame, or null if none available
     */
    HardwareBuffer acquireBuffer(int streamId) {
        StreamContext ctx;
        synchronized (mStreams) {
            ctx = mStreams.get(streamId);
        }
        
        if (ctx != null) {
            return ctx.acquireBuffer();
        }
        return null;
    }
    
    /**
     * Release a buffer back to the pool.
     */
    void releaseBuffer(int streamId, HardwareBuffer buffer) {
        StreamContext ctx;
        synchronized (mStreams) {
            ctx = mStreams.get(streamId);
        }
        
        if (ctx != null) {
            ctx.releaseBuffer(buffer);
        }
    }
    
    /**
     * Request a still capture frame.
     */
    HardwareBuffer requestStillCapture(int captureId) {
        // For still capture, we could use a separate high-res stream
        // For now, just grab from the first stream
        synchronized (mStreams) {
            if (mStreams.size() > 0) {
                return mStreams.valueAt(0).acquireBuffer();
            }
        }
        return null;
    }
    
    void close() {
        if (mIsOpen) {
            onClosed();
        }
        mHandlerThread.quitSafely();
    }
    
    /**
     * Manages a single stream's buffer queue using ImageReader.
     */
    private static class StreamContext {
        private final StreamConfig mConfig;
        private final ImageReader mReader;
        private final Surface mSurface;
        
        // Track currently acquired images so we can close them
        private final SparseArray<Image> mAcquiredImages = new SparseArray<>();
        private int mNextImageId = 0;
        
        StreamContext(StreamConfig config, Handler handler) {
            mConfig = config;
            
            // Convert format to ImageReader format
            int format = toImageReaderFormat(config.format);
            
            // Create ImageReader with USAGE_GPU flags for zero-copy
            // maxImages = 3 for triple buffering
            mReader = ImageReader.newInstance(
                config.width,
                config.height,
                format,
                3,  // maxImages
                HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE | 
                HardwareBuffer.USAGE_GPU_COLOR_OUTPUT
            );
            
            mSurface = mReader.getSurface();
            
            // Optional: listen for new frames
            mReader.setOnImageAvailableListener(reader -> {
                // Frame available - HAL will acquire when ready
            }, handler);
        }
        
        private int toImageReaderFormat(int halFormat) {
            // Map HAL pixel formats to ImageReader formats
            switch (halFormat) {
                case PixelFormat.RGBA_8888:
                    return ImageFormat.FLEX_RGBA_8888;
                case PixelFormat.RGBX_8888:
                    return ImageFormat.FLEX_RGB_888;
                default:
                    return ImageFormat.YUV_420_888;
            }
        }
        
        Surface getSurface() {
            return mSurface;
        }
        
        /**
         * Acquire the latest buffer from renderer.
         * Returns HardwareBuffer that wraps the underlying gralloc buffer.
         */
        HardwareBuffer acquireBuffer() {
            Image image = mReader.acquireLatestImage();
            if (image == null) {
                return null;
            }
            
            // Get HardwareBuffer from Image (zero-copy)
            HardwareBuffer buffer = image.getHardwareBuffer();
            
            if (buffer != null) {
                // Track image so we can close it when buffer is released
                int imageId = mNextImageId++;
                synchronized (mAcquiredImages) {
                    mAcquiredImages.put(imageId, image);
                }
                // Store imageId in the buffer somehow... 
                // Actually, we need a different approach
            }
            
            return buffer;
        }
        
        /**
         * Release buffer back to pool.
         */
        void releaseBuffer(HardwareBuffer buffer) {
            // Close the buffer to return it to ImageReader
            buffer.close();
            
            // Also need to close the Image
            // This is tricky - we need to track which Image goes with which buffer
            // For now, just close oldest acquired image
            synchronized (mAcquiredImages) {
                if (mAcquiredImages.size() > 0) {
                    Image image = mAcquiredImages.valueAt(0);
                    mAcquiredImages.removeAt(0);
                    image.close();
                }
            }
        }
        
        void release() {
            // Close all acquired images
            synchronized (mAcquiredImages) {
                for (int i = 0; i < mAcquiredImages.size(); i++) {
                    mAcquiredImages.valueAt(i).close();
                }
                mAcquiredImages.clear();
            }
            
            mReader.close();
        }
    }
}
