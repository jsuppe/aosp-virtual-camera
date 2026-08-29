/*
 * VirtualCameraNative - java facade over libvirtualcamera_relay_jni.
 *
 * Bridges VirtualCameraService to the vendor HAL across the frozen
 * android.hardware.camera.virtual AIDL:
 *   down:  setProducerAvailable(), and per-frame pushes (inside the JNI lib)
 *   up:    onStreamsConfigured / onCameraClosed / HAL-death notifications
 */
package com.android.server.camera.virtual;

import android.util.Log;
import android.view.Surface;

public final class VirtualCameraNative {
    private static final String TAG = "VirtualCameraNative";

    /** Callbacks from the vendor HAL, delivered on a binder thread. */
    public interface Listener {
        void onHalStreamsConfigured(int width, int height, int fps);
        void onHalCameraClosed();
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
            onStreamsConfiguredFromHal(0, 0, 0);
            onCameraClosedFromHal();
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

    public static void setProducerAvailable(boolean available) {
        if (!sLoaded) return;
        try {
            nativeSetProducerAvailable(available);
        } catch (Throwable t) {
            Log.w(TAG, "setProducerAvailable failed", t);
        }
    }

    public static Surface createSurface(int width, int height) {
        if (!sLoaded) return null;
        return nativeCreateSurface(width, height);
    }

    public static void releaseSurface() {
        if (!sLoaded) return;
        nativeReleaseSurface();
    }

    // ---- called from JNI (binder threads) ----
    static void onStreamsConfiguredFromHal(int width, int height, int fps) {
        Listener l = sListener;
        if (l != null) l.onHalStreamsConfigured(width, height, fps);
    }

    static void onCameraClosedFromHal() {
        Listener l = sListener;
        if (l != null) l.onHalCameraClosed();
    }

    static void onHalDiedFromNative() {
        Listener l = sListener;
        if (l != null) l.onHalDied();
    }

    private static native boolean nativeIsHalUp();
    private static native void nativeSetProducerAvailable(boolean available);
    private static native Surface nativeCreateSurface(int width, int height);
    private static native void nativeReleaseSurface();
}
