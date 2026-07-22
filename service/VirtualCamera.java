/*
 * VirtualCamera - Individual registered virtual camera
 *
 * A13 relay design: pure state-holder + relay between HAL notifications and
 * the renderer app callback. The HAL owns the BufferQueues; Surfaces pass
 * through here untouched.
 */
package com.android.server.camera.virtual;

import android.hardware.virtualcamera.IVirtualCameraCallback;
import android.hardware.virtualcamera.VirtualCameraConfig;
import android.hardware.virtualcamera.StreamConfig;
import android.os.RemoteException;
import android.util.Log;
import android.view.Surface;

class VirtualCamera {
    private static final String TAG = "VirtualCamera";

    private final int mCameraId;
    private final VirtualCameraConfig mConfig;
    private final IVirtualCameraCallback mCallback;

    private volatile boolean mIsOpen = false;
    private volatile boolean mIsCapturing = false;

    VirtualCamera(int cameraId, VirtualCameraConfig config, IVirtualCameraCallback callback) {
        mCameraId = cameraId;
        mConfig = config;
        mCallback = callback;
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

    void onStreamsConfigured(StreamConfig[] streams, Surface[] surfaces) {
        try {
            mCallback.onStreamsConfigured(streams, surfaces);
        } catch (RemoteException e) {
            Log.e(TAG, "Failed to relay surfaces to renderer", e);
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
    }

    void close() {
        if (mIsOpen) {
            onClosed();
        }
    }
}
