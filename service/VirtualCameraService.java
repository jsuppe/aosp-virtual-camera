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
import java.util.concurrent.atomic.AtomicBoolean;

public class VirtualCameraService extends IVirtualCameraService.Stub
        implements VirtualCameraNative.Listener {
    private static final String TAG = "VirtualCameraService";

    public static final String SERVICE_NAME = "virtual_camera";
    public static final String MANAGER_SERVICE_NAME = "virtual_camera_manager";

    private final Context mContext;
    private final AtomicInteger mNextCameraId = new AtomicInteger(1000);
    private final Object mLock = new Object();
    private final SparseArray<VirtualCamera> mCameras = new SparseArray<>();
    // Vendor-APEX HAL path: each registered producer occupies a HAL slot
    // (Camera2 device 100 + slot). Guarded by mLock. Sized to the HAL's
    // getMaxCameras() at first use (1 for a V1/V2 HAL).
    private static final int MAX_SLOTS = 4;
    private final java.util.ArrayList<LifecycleSlot<VirtualCamera>> mSlots = new java.util.ArrayList<>();
    private final java.util.ArrayList<java.util.concurrent.ExecutorService> mLifecycle = new java.util.ArrayList<>();
    private final java.util.concurrent.ScheduledExecutorService mRecovery =
            java.util.concurrent.Executors.newSingleThreadScheduledExecutor(r -> new Thread(r, "vcam-recovery"));
    private final AtomicInteger mRecoveryRound = new AtomicInteger();
    private final SparseArray<Integer> mSlotByCameraId = new SparseArray<>();

    private final ManagerImpl mManager = new ManagerImpl();
    // HAL availability listener (guarded by mLock). Pushed on every 0<->N
    // producer transition so the HAL can add/remove the camera device.
    private IVirtualCameraHalListener mHalListener;

    public VirtualCameraService(Context context) {
        mContext = context;
        for (int s = 0; s < MAX_SLOTS; s++) {
            final int slot = s;
            mSlots.add(new LifecycleSlot<>());
            mLifecycle.add(java.util.concurrent.Executors.newSingleThreadExecutor(
                    r -> new Thread(r, "vcam-lifecycle-" + slot)));
        }
        // Stable-AIDL boundary (vendor-APEX HAL): receive stream lifecycle
        // callbacks and push availability/frames via the JNI relay.
        VirtualCameraNative.setListener(this);
        Log.i(TAG, "VirtualCameraService created");
    }

    // ============ VirtualCameraNative.Listener (vendor-APEX HAL path) ============

    @Override
    public void onHalStreamsConfigured(int slot, int width, int height, int fps) {
        if (slot < 0 || slot >= MAX_SLOTS) return;
        final long connection = VirtualCameraNative.callbackEpoch();
        final long configuration = VirtualCameraNative.callbackConfiguration();
        final long registration = VirtualCameraNative.callbackRegistration();
        if (!VirtualCameraNative.isCurrentCallbackConnection(connection)) return;
        final VirtualCamera owner;
        final long event;
        synchronized (mLock) {
            owner = mSlots.get(slot).current();
            if (owner == null || owner.getCameraId() != registration) return;
            event = mSlots.get(slot).nextConfigurationEvent(owner, connection, configuration, false);
            if (event == 0) return;
        }
        if (owner == null) return;
        mLifecycle.get(slot).execute(() -> {
            if (!isCurrentEvent(slot, owner, event)) return;
            Surface surface = VirtualCameraNative.createSurfaceForEpoch(slot, width, height, connection, configuration);
            if (surface == null) return; // obsolete HAL connection
            try {
                if (!isCurrentEvent(slot, owner, event)) return; // newer work/removal won
                StreamConfig sc = new StreamConfig();
                sc.streamId = 0; sc.width = width; sc.height = height;
                sc.format = 1; sc.fps = fps; sc.configurationToken = configuration; sc.connectionToken = connection;
                owner.connection = connection; owner.configuration = configuration;
                owner.onOpened();
                owner.onStreamsConfigured(new StreamConfig[] { sc }, new Surface[] { surface });
                owner.onCaptureStarted(fps);
            } finally {
                surface.release(); // producer owns its parcelled wrapper
            }
        });
    }

    @Override
    public void onHalCameraClosed(int slot) {
        if (slot < 0 || slot >= MAX_SLOTS) return;
        final long connection = VirtualCameraNative.callbackEpoch();
        final long configuration = VirtualCameraNative.callbackConfiguration();
        final long registration = VirtualCameraNative.callbackRegistration();
        if (!VirtualCameraNative.isCurrentCallbackConnection(connection)) return;
        final VirtualCamera owner;
        final long event;
        synchronized (mLock) {
            owner = mSlots.get(slot).current();
            if (owner == null || owner.getCameraId() != registration) return;
            event = mSlots.get(slot).nextConfigurationEvent(owner, connection, configuration, true);
            if (event == 0) return;
        }
        if (owner == null) return; // removal already owns cleanup
        mLifecycle.get(slot).execute(() -> {
            if (!isCurrentEvent(slot, owner, event) ||
                    !VirtualCameraNative.isCurrentCallbackConnection(connection)) return;
            owner.onCaptureStopped();
            owner.onClosed();
            VirtualCameraNative.releaseSurfaceForEpoch(slot, connection, configuration);
        });
    }

    @Override
    public void onHalDied() {
        final long connection = VirtualCameraNative.callbackEpoch();
        for (int slot = 0; slot < MAX_SLOTS; ++slot) {
            final int s = slot;
            mLifecycle.get(s).execute(() -> VirtualCameraNative.releaseSurfaceForEpoch(s, connection));
        }
        final int round = mRecoveryRound.incrementAndGet();
        recoverHal(round, 0);
    }

    private void recoverHal(int round, int attempt) {
        mRecovery.schedule(() -> {
            if (mRecoveryRound.get() != round) return;
            if (!VirtualCameraNative.isHalUp()) {
                if (attempt < 29) recoverHal(round, attempt + 1);
                return;
            }
            for (int s = 0; s < MAX_SLOTS; s++) {
                final int slot = s;
                final VirtualCamera owner = cameraInSlot(slot);
                if (owner == null) continue;
                mLifecycle.get(slot).execute(() -> {
                    if (mRecoveryRound.get() == round && mSlots.get(slot).owns(owner)) {
                        VirtualCameraNative.registerProducer(slot, owner.getCameraId(), owner.descriptor());
                    }
                });
            }
        }, 1, java.util.concurrent.TimeUnit.SECONDS);
    }

    private VirtualCamera cameraInSlot(int slot) {
        synchronized (mLock) {
            return (slot >= 0 && slot < MAX_SLOTS) ? mSlots.get(slot).current() : null;
        }
    }

    private boolean isCurrentEvent(int slot, VirtualCamera owner, long event) {
        synchronized (mLock) {
            return mSlots.get(slot).isCurrentEvent(owner, event);
        }
    }

    /** Lowest free slot within what the HAL supports, or -1. Caller holds mLock. */
    private int allocateSlotLocked(VirtualCamera owner, int max) {
        for (int s = 0; s < max; s++) {
            if (mSlots.get(s).reserve(owner)) return s;
        }
        return -1;
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
        config = ProducerCapabilities.normalize(config);
        final VirtualCamera camera = new VirtualCamera(id, config, callback);
        final AtomicBoolean dead = new AtomicBoolean();
        final int max = Math.max(1, Math.min(MAX_SLOTS, VirtualCameraNative.getMaxCameras()));

        try {
            callback.asBinder().linkToDeath(() -> {
                dead.set(true);
                Log.w(TAG, "Renderer for camera " + id + " died, unregistering");
                removeCamera(id);
            }, 0);
        } catch (RemoteException e) {
            Log.e(TAG, "Renderer already dead", e);
            return -1;
        }

        boolean becameAvailable;
        int slot;
        synchronized (mLock) {
            if (dead.get() || !callback.asBinder().isBinderAlive()) return -1;
            becameAvailable = (mCameras.size() == 0);
            mCameras.put(id, camera);
            slot = allocateSlotLocked(camera, max);
            if (slot >= 0) {
                mSlotByCameraId.put(id, slot);
            }
        }
        if (slot < 0) {
            removeCamera(id);
            throw new IllegalStateException("no free producer slot");
        }
        Log.i(TAG, "Registered virtual camera " + id + " (\"" + config.name
                + "\") for uid " + Binder.getCallingUid()
                + (slot >= 0 ? " -> HAL slot " + slot + " (Camera2 id " + (100 + slot) + ")"
                             : " -> no free HAL slot (registered, not exposed)"));
        if (slot >= 0) {
            final int assigned = slot;
            mLifecycle.get(slot).execute(() -> {
                if (mSlots.get(assigned).owns(camera) && !VirtualCameraNative.registerProducer(assigned, camera.getCameraId(), camera.descriptor())) {
                    if (!VirtualCameraNative.isHalUp()) {
                        // Valid pending registration stays hidden until the matched HAL accepts its descriptor.
                        recoverHal(mRecoveryRound.incrementAndGet(), 0);
                    } else {
                        Log.e(TAG, "Descriptor rejected by matched HAL"); removeCamera(camera.getCameraId());
                    }
                }
            });
        }
        if (becameAvailable) {
            notifyHalAvailability(true);
        }
        return id;
    }

    @Override
    public void unregisterCamera(int cameraId) {
        VirtualCamera owner = getCamera(cameraId);
        if (owner != null && owner.uid != Binder.getCallingUid()) throw new SecurityException("not producer owner");
        removeCamera(cameraId);
    }

    @Override
    public void setProducerReady(IVirtualCameraCallback callback, long connection, long token, boolean ready) {
        final int uid = Binder.getCallingUid();
        VirtualCamera owner = null; int slot = -1;
        synchronized (mLock) {
            for (int i=0;i<mCameras.size();i++) {
                VirtualCamera c = mCameras.valueAt(i);
                if (c.uid == uid && c.hasCallback(callback) && c.connection == connection && connection != 0 && c.configuration == token && token > 0) {
                    Integer assigned = mSlotByCameraId.get(c.getCameraId());
                    if (assigned != null && mSlots.get(assigned).owns(c)) { owner=c; slot=assigned; break; }
                }
            }
        }
        if (owner == null || !VirtualCameraNative.setConfigurationReady(slot, owner.connection, token, ready))
            throw new IllegalArgumentException("stale or unowned configuration readiness");
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
        final VirtualCamera camera;
        final Integer assigned;
        synchronized (mLock) {
            camera = mCameras.get(cameraId);
            assigned = mSlotByCameraId.get(cameraId);
        }
        if (camera == null) return;
        if (assigned != null) {
            final int slot = assigned;
            LifecycleSlot<VirtualCamera> state = mSlots.get(slot);
            synchronized (mLock) {
                if (mCameras.get(cameraId) != camera || !state.beginClose(camera)) return;
                mCameras.remove(cameraId);
                mSlotByCameraId.remove(cameraId);
            }
            mLifecycle.get(slot).execute(() -> {
                // Reservation persists until queued RPC/producer cleanup
                // finishes. No Java monitor is held across a Binder call.
                VirtualCameraNative.removeProducer(slot, camera.getCameraId());
                VirtualCameraNative.releaseSurface(slot);
                camera.close();
                synchronized (mLock) { state.finishClose(camera); }
            });
            notifyHalAvailability(false);
            return;
        }
        boolean becameUnavailable = false;
        synchronized (mLock) {
            if (mCameras.get(cameraId) == camera) {
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
            available = mCameras.size() > 0; // do not replay an obsolete caller snapshot
        }
        // (The vendor-APEX HAL path is driven per slot in register/remove.)
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
