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

    public static void setCameraPresent(int slot, boolean present) {
        if (!sLoaded) return;
        try {
            nativeSetCameraPresent(slot, present);
        } catch (Throwable t) {
            Log.w(TAG, "setCameraPresent failed", t);
        }
    }

    public static Surface createSurface(int slot, int width, int height) {
        if (!sLoaded) return null;
        return nativeCreateSurface(slot, width, height);
    }

    public static void releaseSurface(int slot) {
        if (!sLoaded) return;
        nativeReleaseSurface(slot);
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
    private static native int nativeGetMaxCameras();
    private static native void nativeSetCameraPresent(int slot, boolean present);
    private static native Surface nativeCreateSurface(int slot, int width, int height);
    private static native void nativeReleaseSurface(int slot);
}
