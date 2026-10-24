/*
 * VirtualCameraNative - java facade over libvirtualcamera_relay_jni.
 *
 * Bridges VirtualCameraService to the vendor HAL across the frozen
 * android.hardware.virtualcamera.hal AIDL (V3: per-camera slots):
 *   down:  setCameraPresent(slot), and per-frame pushes (inside the JNI lib)
 *   up:    onStreamsConfigured(slot) / onCameraClosed(slot) / HAL-death
 */
package com.android.server.camera.virtual;

import android.util.Log;
import android.view.Surface;

public final class VirtualCameraNative {
    private static final String TAG = "VirtualCameraNative";

    /** Callbacks from the vendor HAL, delivered on a binder thread. */
    public interface Listener {
        void onHalStreamsConfigured(int slot, int width, int height, int fps);
        void onHalCameraClosed(int slot);
        void onHalDied();
    }

    private static volatile Listener sListener;
    private static volatile boolean sLoaded = false;

    private VirtualCameraNative() {}

    public static void setListener(Listener listener) {
        sListener = listener;
        // Reference the native-called methods so R8 cannot strip them from
        // services.jar (they are only invoked from JNI).
        if (Boolean.getBoolean("vcam.debug.invoke_callbacks")) {
            onStreamsConfiguredFromHal(0, 0, 0, 0);
            onCameraClosedFromHal(0);
            onHalDiedFromNative();
        }
        ensureLoaded();
    }

    private static synchronized void ensureLoaded() {
        if (sLoaded) return;
        try {
            System.loadLibrary("virtualcamera_relay_jni");
            sLoaded = true;
            Log.i(TAG, "relay JNI loaded");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "relay JNI unavailable: " + e.getMessage());
        }
    }

    public static boolean isHalUp() {
        return sLoaded && nativeIsHalUp();
    }

    /** Number of virtual cameras the HAL can expose (1 for a V1/V2 HAL). */
    public static int getMaxCameras() {
        if (!sLoaded) return 0;
        try {
            return nativeGetMaxCameras();
        } catch (Throwable t) {
            return 0;
        }
    }

    public static boolean registerProducer(int slot, long registration, int[] descriptor) {
        return sLoaded && nativeRegisterProducer(slot, registration, descriptor);
    }
    public static void removeProducer(int slot, long registration) {
        if (sLoaded) nativeRemoveProducer(slot, registration);
    }
    public static boolean setConfigurationReady(int slot, long connection, long token, boolean ready) {
        return sLoaded && nativeSetConfigurationReady(slot, connection, token, ready);
    }

    public static Surface createSurface(int slot, int width, int height) {
        if (!sLoaded) return null;
        return nativeCreateSurfaceForEpoch(slot, width, height, 0, 0); // rejected on V4
    }

    public static void releaseSurface(int slot) {
        if (!sLoaded) return;
        nativeReleaseSurface(slot);
    }

    // Snapshot the JNI callback scope before deferring work to a lifecycle
    // executor. Scoped native entry points restore it on that worker thread.
    public static long callbackEpoch() { return sLoaded ? nativeCallbackEpoch() : 0; }
    public static long callbackConfiguration() { return sLoaded ? nativeCallbackConfiguration() : 0; }
    public static long callbackRegistration() { return sLoaded ? nativeCallbackRegistration() : 0; }
    public static boolean isCurrentCallbackConnection(long epoch) {
        return sLoaded && epoch != 0 && nativeIsCurrentConnection(epoch);
    }
    public static Surface createSurfaceForEpoch(int slot, int width, int height, long epoch, long token) {
        return sLoaded && epoch != 0 && token > 0 ? nativeCreateSurfaceForEpoch(slot, width, height, epoch, token) : null;
    }
    public static void releaseSurfaceForEpoch(int slot, long epoch, long token) {
        if (sLoaded && epoch != 0 && token > 0) nativeReleaseSurfaceForEpoch(slot, epoch, token);
    }
    // HAL death invalidates every configuration owned by that connection.
    public static void releaseSurfaceForEpoch(int slot, long epoch) {
        if (sLoaded && epoch != 0) nativeReleaseSurfaceForEpoch(slot, epoch, 0);
    }

    // ---- called from JNI (binder threads) ----
    static void onStreamsConfiguredFromHal(int slot, int width, int height, int fps) {
        Listener l = sListener;
        if (l != null) l.onHalStreamsConfigured(slot, width, height, fps);
    }

    static void onCameraClosedFromHal(int slot) {
        Listener l = sListener;
        if (l != null) l.onHalCameraClosed(slot);
    }

    static void onHalDiedFromNative() {
        Listener l = sListener;
        if (l != null) l.onHalDied();
    }

    private static native boolean nativeIsHalUp();
    private static native boolean nativeRegisterProducer(int slot, long registration, int[] descriptor);
    private static native void nativeRemoveProducer(int slot, long registration);
    private static native boolean nativeSetConfigurationReady(int slot, long connection, long token, boolean ready);
    private static native int nativeGetMaxCameras();
    private static native void nativeReleaseSurface(int slot);
    private static native long nativeCallbackEpoch();
    private static native long nativeCallbackConfiguration();
    private static native long nativeCallbackRegistration();
    private static native boolean nativeIsCurrentConnection(long epoch);
    private static native Surface nativeCreateSurfaceForEpoch(int slot, int width, int height, long epoch, long token);
    private static native void nativeReleaseSurfaceForEpoch(int slot, long epoch, long token);
}
