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

public class VirtualCameraService extends IVirtualCameraService.Stub
        implements VirtualCameraNative.Listener {
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
        // Stable-AIDL boundary (vendor-APEX HAL): receive stream lifecycle
        // callbacks and push availability/frames via the JNI relay.
        VirtualCameraNative.setListener(this);
        Log.i(TAG, "VirtualCameraService created");
    }

    // ============ VirtualCameraNative.Listener (vendor-APEX HAL path) ============

    @Override
    public void onHalStreamsConfigured(int width, int height, int fps) {
        VirtualCamera camera = firstCamera();
        if (camera == null) {
            Log.w(TAG, "HAL streams configured but no producer registered");
            return;
        }
        Surface surface = VirtualCameraNative.createSurface(width, height);
        if (surface == null) {
            Log.e(TAG, "Failed to create relay Surface");
            return;
        }
        StreamConfig sc = new StreamConfig();
        sc.streamId = 0;
        sc.width = width;
        sc.height = height;
        sc.format = 1; // RGBA_8888
        sc.fps = fps;
        Log.i(TAG, "Relaying platform BufferQueue Surface (" + width + "x"
                + height + "@" + fps + ") to producer app");
        camera.onOpened();
        camera.onStreamsConfigured(new StreamConfig[] { sc },
                new Surface[] { surface });
        camera.onCaptureStarted(fps);
    }

    @Override
    public void onHalCameraClosed() {
        VirtualCamera camera = firstCamera();
        if (camera != null) {
            camera.onCaptureStopped();
            camera.onClosed();
        }
        VirtualCameraNative.releaseSurface();
    }

    @Override
    public void onHalDied() {
        // APEX update or HAL crash: tear down; availability re-pushed when
        // the HAL returns (next isHalUp()/setProducerAvailable call).
        VirtualCameraNative.releaseSurface();
        Log.w(TAG, "vendor HAL died; relay torn down");
        // Re-sync availability once it comes back (best-effort poll).
        final boolean available;
        synchronized (mLock) {
            available = mCameras.size() > 0;
        }
        new Thread(() -> {
            for (int i = 0; i < 30; i++) {
                try { Thread.sleep(1000); } catch (InterruptedException ignored) { }
                if (VirtualCameraNative.isHalUp()) {
                    VirtualCameraNative.setProducerAvailable(available);
                    Log.i(TAG, "vendor HAL back; availability re-synced=" + available);
                    return;
                }
            }
        }, "vcam-hal-resync").start();
    }

    private VirtualCamera firstCamera() {
        synchronized (mLock) {
            return mCameras.size() > 0 ? mCameras.valueAt(0) : null;
        }
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
        // Vendor-APEX HAL path (no-op when the stable HAL isn't present).
        VirtualCameraNative.setProducerAvailable(available);
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
