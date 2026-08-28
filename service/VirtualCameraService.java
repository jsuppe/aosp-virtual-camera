/*
 * VirtualCameraService - System Service Implementation
 *
 * Implements both:
 * - IVirtualCameraService: renderer apps register as frame producers
 * - IVirtualCameraManager: HAL relays stream Surfaces and lifecycle events
 *
 * A13 relay design: the HAL owns the BufferQueues. When a consumer app
 * configures camera streams, the HAL calls notifyStreamsConfigured() with the
 * producer-side Surfaces, and this service relays them to the registered
 * renderer app via IVirtualCameraCallback.onStreamsConfigured(). Frames then
 * flow app -> Surface -> HAL BufferQueue without crossing system_server.
 *
 * Location: built as "virtual-camera-service" lib, statically linked into services.
 */
package com.android.server.camera.virtual;

import android.content.Context;
import android.hardware.virtualcamera.IVirtualCameraService;
import android.hardware.virtualcamera.IVirtualCameraManager;
import android.hardware.virtualcamera.IVirtualCameraCallback;
import android.hardware.virtualcamera.IVirtualCameraHalListener;
import android.hardware.virtualcamera.VirtualCameraConfig;
import android.hardware.virtualcamera.StreamConfig;
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;
import android.util.Log;
import android.util.SparseArray;
import android.view.Surface;

import com.android.server.SystemService;

import java.util.concurrent.atomic.AtomicInteger;

public class VirtualCameraService extends IVirtualCameraService.Stub {
    private static final String TAG = "VirtualCameraService";

    public static final String SERVICE_NAME = "virtual_camera";
    public static final String MANAGER_SERVICE_NAME = "virtual_camera_manager";

    private final Context mContext;
    private final AtomicInteger mNextCameraId = new AtomicInteger(1000);
    private final Object mLock = new Object();
    private final SparseArray<VirtualCamera> mCameras = new SparseArray<>();

    private final ManagerImpl mManager = new ManagerImpl();
    // HAL availability listener (guarded by mLock). Pushed on every 0<->N
    // producer transition so the HAL can add/remove the camera device.
    private IVirtualCameraHalListener mHalListener;

    public VirtualCameraService(Context context) {
        mContext = context;
        Log.i(TAG, "VirtualCameraService created");
    }

    IBinder getManagerBinder() {
        return mManager;
    }

    // ============ IVirtualCameraService (renderer apps) ============

    @Override
    public int registerCamera(VirtualCameraConfig config, IVirtualCameraCallback callback) {
        if (config == null || callback == null) {
            throw new IllegalArgumentException("config and callback are required");
        }
        final int id = mNextCameraId.getAndIncrement();
        final VirtualCamera camera = new VirtualCamera(id, config, callback);

        try {
            callback.asBinder().linkToDeath(() -> {
                Log.w(TAG, "Renderer for camera " + id + " died, unregistering");
                removeCamera(id);
            }, 0);
        } catch (RemoteException e) {
            Log.e(TAG, "Renderer already dead", e);
            return -1;
        }

        boolean becameAvailable;
        synchronized (mLock) {
            becameAvailable = (mCameras.size() == 0);
            mCameras.put(id, camera);
        }
        Log.i(TAG, "Registered virtual camera " + id + " (\"" + config.name
                + "\") for uid " + Binder.getCallingUid());
        if (becameAvailable) {
            notifyHalAvailability(true);
        }
        return id;
    }

    @Override
    public void unregisterCamera(int cameraId) {
        removeCamera(cameraId);
    }

    @Override
    public int[] getRegisteredCameras() {
        synchronized (mLock) {
            int[] ids = new int[mCameras.size()];
            for (int i = 0; i < mCameras.size(); i++) {
                ids[i] = mCameras.keyAt(i);
            }
            return ids;
        }
    }

    @Override
    public boolean isCameraInUse(int cameraId) {
        VirtualCamera camera = getCamera(cameraId);
        return camera != null && camera.isInUse();
    }

    private VirtualCamera getCamera(int cameraId) {
        synchronized (mLock) {
            return mCameras.get(cameraId);
        }
    }

    private void removeCamera(int cameraId) {
        VirtualCamera camera;
        boolean becameUnavailable = false;
        synchronized (mLock) {
            camera = mCameras.get(cameraId);
            if (camera != null) {
                mCameras.remove(cameraId);
                becameUnavailable = (mCameras.size() == 0);
            }
        }
        if (camera != null) {
            camera.close();
            Log.i(TAG, "Unregistered virtual camera " + cameraId);
        }
        if (becameUnavailable) {
            notifyHalAvailability(false);
        }
    }

    /** Push availability to the HAL listener; never call while holding mLock. */
    private void notifyHalAvailability(boolean available) {
        IVirtualCameraHalListener listener;
        synchronized (mLock) {
            listener = mHalListener;
        }
        if (listener == null) {
            return;
        }
        try {
            listener.onProducerAvailabilityChanged(available);
            Log.i(TAG, "HAL notified: producer availability -> " + available);
        } catch (RemoteException e) {
            Log.w(TAG, "HAL listener dead, dropping", e);
            synchronized (mLock) {
                if (mHalListener == listener) mHalListener = null;
            }
        }
    }

    // ============ IVirtualCameraManager (HAL) ============

    private class ManagerImpl extends IVirtualCameraManager.Stub {
        @Override
        public int[] getRegisteredCameraIds() {
            return getRegisteredCameras();
        }

        @Override
        public VirtualCameraConfig getCameraConfig(int cameraId) {
            VirtualCamera camera = getCamera(cameraId);
            return camera != null ? camera.getConfig() : null;
        }

        @Override
        public void notifyCameraOpened(int cameraId) {
            Log.i(TAG, "HAL: camera " + cameraId + " opened");
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) camera.onOpened();
        }

        @Override
        public void notifyStreamsConfigured(int cameraId, StreamConfig[] streams,
                Surface[] surfaces) {
            Log.i(TAG, "HAL: camera " + cameraId + " streams configured ("
                    + (streams != null ? streams.length : 0) + " streams)");
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) camera.onStreamsConfigured(streams, surfaces);
        }

        @Override
        public void notifyCaptureStarted(int cameraId, int frameRate) {
            Log.i(TAG, "HAL: camera " + cameraId + " capture started @" + frameRate);
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) camera.onCaptureStarted(frameRate);
        }

        @Override
        public void notifyCaptureStopped(int cameraId) {
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) camera.onCaptureStopped();
        }

        @Override
        public void setHalListener(IVirtualCameraHalListener listener) {
            boolean available;
            synchronized (mLock) {
                mHalListener = listener;
                available = (mCameras.size() > 0);
            }
            Log.i(TAG, "HAL listener registered (current availability=" + available + ")");
            if (listener != null) {
                try {
                    listener.asBinder().linkToDeath(() -> {
                        synchronized (mLock) {
                            if (mHalListener == listener) mHalListener = null;
                        }
                        Log.w(TAG, "HAL listener died");
                    }, 0);
                    // Sync the current state immediately.
                    listener.onProducerAvailabilityChanged(available);
                } catch (RemoteException e) {
                    Log.w(TAG, "HAL listener register failed", e);
                    synchronized (mLock) {
                        if (mHalListener == listener) mHalListener = null;
                    }
                }
            }
        }

        @Override
        public void notifyCameraClosed(int cameraId) {
            Log.i(TAG, "HAL: camera " + cameraId + " closed");
            VirtualCamera camera = getCamera(cameraId);
            if (camera != null) camera.onClosed();
        }
    }

    // ============ SystemService Lifecycle ============

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
            Log.i(TAG, "VirtualCameraService published (" + SERVICE_NAME + ", "
                    + MANAGER_SERVICE_NAME + ")");
        }
    }
}
