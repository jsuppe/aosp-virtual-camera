/*
 * VirtualCameraService - System Service Implementation
 * 
 * Implements both:
 * - IVirtualCameraService: for renderer apps to register cameras
 * - IVirtualCameraManager: for HAL to communicate with service
 * 
 * Location: frameworks/base/services/core/java/com/android/server/camera/virtual/
 */
package com.android.server.camera.virtual;

import android.annotation.NonNull;
import android.content.Context;
import android.hardware.HardwareBuffer;
import android.hardware.camera.virtual.IVirtualCameraService;
import android.hardware.camera.virtual.IVirtualCameraManager;
import android.hardware.camera.virtual.IVirtualCameraCallback;
import android.hardware.camera.virtual.VirtualCameraConfig;
import android.hardware.camera.virtual.StreamConfig;
import android.os.IBinder;
import android.os.RemoteException;
import android.util.Log;
import android.util.SparseArray;

import com.android.server.SystemService;

import java.util.concurrent.atomic.AtomicInteger;

/**
 * System service managing virtual cameras.
 * 
 * Exposes two AIDL interfaces:
 * 1. IVirtualCameraService - for apps to register as camera renderers
 * 2. IVirtualCameraManager - for Camera HAL to get buffers
 */
public class VirtualCameraService extends IVirtualCameraService.Stub {
    private static final String TAG = "VirtualCameraService";
    
    // Service names
    public static final String SERVICE_NAME = "virtual_camera";
    public static final String MANAGER_SERVICE_NAME = "virtual_camera_manager";
    
    private final Context mContext;
    private final AtomicInteger mNextCameraId = new AtomicInteger(1000);
    private final SparseArray<VirtualCamera> mCameras = new SparseArray<>();
    
    // Manager interface for HAL
    private final ManagerImpl mManager = new ManagerImpl();
    
    public VirtualCameraService(Context context) {
        mContext = context;
        Log.i(TAG, "VirtualCameraService created");
    }
    
    /**
     * Get the manager binder for HAL communication.
     */
    IBinder getManagerBinder() {
        return mManager;
    }
    
    /**
     * Called when system services are ready.
     */
    void onSystemServicesReady() {
        Log.i(TAG, "System services ready");
    }
    
    /**
     * Called when boot is complete.
     */
    void onBootCompleted() {
        Log.i(TAG, "Boot completed, VirtualCameraService ready");
    }
    
    // ============ SystemService Lifecycle ============
    
    /**
     * Lifecycle class for SystemServiceManager integration.
     */
    public static class Lifecycle extends SystemService {
        private VirtualCameraService mService;
        
        public Lifecycle(Context context) {
            super(context);
        }
        
        @Override
        public void onStart() {
            mService = new VirtualCameraService(getContext());
            publishBinderService(SERVICE_NAME, mService);
            publishBinderService(MANAGER_SERVICE_NAME, mService.getManagerBinder());
            Log.i(TAG, "VirtualCameraService published");
        }
        
        @Override
        public void onBootPhase(int phase) {
            if (phase == SystemService.PHASE_SYSTEM_SERVICES_READY) {
                mService.onSystemServicesReady();
            } else if (phase == SystemService.PHASE_BOOT_COMPLETED) {
                mService.onBootCompleted();
            }
        }
        
        @Override
        public void onUserStarting(@NonNull TargetUser user) {
            // Per-user initialization if needed
        }
    }
    
    // ============ IVirtualCameraService (for renderer apps) ============
    
    @Override
    public int registerCamera(VirtualCameraConfig config, IVirtualCameraCallback callback) {
        // Validate config
        if (config.maxWidth <= 0 || config.maxHeight <= 0) {
            Log.e(TAG, "Invalid camera config: dimensions must be positive");
            return -1;
        }
        
        if (config.supportedFormats == null || config.supportedFormats.length == 0) {
            Log.e(TAG, "Invalid camera config: must specify at least one format");
            return -1;
        }
        
        int cameraId = mNextCameraId.getAndIncrement();
        
        VirtualCamera camera = new VirtualCamera(cameraId, config, callback);
        
        synchronized (mCameras) {
            mCameras.put(cameraId, camera);
        }
        
        // Link to death to cleanup if renderer crashes
        try {
            callback.asBinder().linkToDeath(() -> {
                Log.w(TAG, "Renderer died, unregistering camera " + cameraId);
                unregisterCamera(cameraId);
            }, 0);
        } catch (RemoteException e) {
            Log.e(TAG, "Failed to link to death", e);
        }
        
        Log.i(TAG, "Registered virtual camera: id=" + cameraId + 
              ", name=" + config.name +
              ", resolution=" + config.maxWidth + "x" + config.maxHeight +
              ", fps=" + config.maxFps);
        
        return cameraId;
    }
    
    @Override
    public void unregisterCamera(int cameraId) {
        VirtualCamera camera;
        synchronized (mCameras) {
            camera = mCameras.removeReturnOld(cameraId);
        }
        
        if (camera != null) {
            camera.close();
            Log.i(TAG, "Unregistered virtual camera: " + cameraId);
        }
    }
    
    @Override
    public int[] getRegisteredCameras() {
        synchronized (mCameras) {
            int[] ids = new int[mCameras.size()];
            for (int i = 0; i < mCameras.size(); i++) {
                ids[i] = mCameras.keyAt(i);
            }
            return ids;
        }
    }
    
    @Override
    public boolean isCameraInUse(int cameraId) {
        synchronized (mCameras) {
            VirtualCamera camera = mCameras.get(cameraId);
            return camera != null && camera.isInUse();
        }
    }
    
    // ============ Helper to get camera ============
    
    private VirtualCamera getCamera(int cameraId) {
        synchronized (mCameras) {
            return mCameras.get(cameraId);
        }
    }
    
    // ============ IVirtualCameraManager implementation (for HAL) ============
    
    private class ManagerImpl extends IVirtualCameraManager.Stub {
        
        @Override
        public int[] getRegisteredCameraIds() {
            return VirtualCameraService.this.getRegisteredCameras();
        }
        
        @Override
        public VirtualCameraConfig getCameraConfig(int cameraId) {
            VirtualCamera camera = getCamera(cameraId);
            return camera != null ? camera.getConfig() : null;
        }
        
        @Override
        public void notifyCameraOpened(int cameraId) {
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) {
                camera.onOpened();
            }
        }
        
        @Override
        public void notifyStreamsConfigured(int cameraId, StreamConfig[] streams) {
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) {
                camera.onStreamsConfigured(streams);
            }
        }
        
        @Override
        public void notifyCaptureStarted(int cameraId, int frameRate) {
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) {
                camera.onCaptureStarted(frameRate);
            }
        }
        
        @Override
        public void notifyCaptureStopped(int cameraId) {
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) {
                camera.onCaptureStopped();
            }
        }
        
        @Override
        public void notifyCameraClosed(int cameraId) {
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) {
                camera.onClosed();
            }
        }
        
        @Override
        public HardwareBuffer acquireBuffer(int cameraId, int streamId) {
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) {
                return camera.acquireBuffer(streamId);
            }
            return null;
        }
        
        @Override
        public void releaseBuffer(int cameraId, int streamId, HardwareBuffer buffer) {
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) {
                camera.releaseBuffer(streamId, buffer);
            }
        }
        
        @Override
        public HardwareBuffer requestStillCapture(int cameraId, int captureId) {
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) {
                return camera.requestStillCapture(captureId);
            }
            return null;
        }
    }
}
