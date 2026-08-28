/*
 * VCamViewer - availability-driven Camera2 consumer for the virtual camera.
 *
 * The virtual camera (id "100") only exists while a producer app is
 * registered with VirtualCameraService, so this viewer is driven by
 * CameraManager.AvailabilityCallback:
 *
 *   - no producer  -> camera 100 absent  -> show "waiting for producer"
 *   - producer registers   -> onCameraAvailable("100") -> open + preview
 *   - producer unregisters -> device removed -> onDisconnected -> waiting
 *
 * Start order no longer matters: start the viewer first and it will attach
 * the moment a producer appears.
 */
package com.example.vcamviewer;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Size;
import android.view.Surface;
import android.view.TextureView;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.util.Arrays;

public class MainActivity extends Activity {
    private static final String TAG = "VCamViewer";
    private static final String VIRTUAL_CAMERA_ID = "100";
    private static final int REQ_CAMERA = 1;

    private TextureView mTextureView;
    private TextView mStatusView;
    private HandlerThread mCameraThread;
    private Handler mCameraHandler;
    private CameraManager mCameraManager;
    private CameraDevice mCamera;
    private CameraCaptureSession mSession;
    private boolean mOpening = false;
    private boolean mSurfaceReady = false;
    private long mFrameCount = 0;

    /** Availability events for camera 100 drive the whole UI. */
    private final CameraManager.AvailabilityCallback mAvailability =
            new CameraManager.AvailabilityCallback() {
        @Override
        public void onCameraAvailable(String cameraId) {
            if (!VIRTUAL_CAMERA_ID.equals(cameraId)) return;
            Log.i(TAG, "Virtual camera AVAILABLE (producer registered)");
            maybeOpenCamera();
        }

        @Override
        public void onCameraUnavailable(String cameraId) {
            if (!VIRTUAL_CAMERA_ID.equals(cameraId)) return;
            // Fires both when we open it ourselves and when the device is
            // removed; removal-while-open is handled via onDisconnected.
            Log.i(TAG, "Virtual camera unavailable (in use or removed)");
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        FrameLayout root = new FrameLayout(this);
        mTextureView = new TextureView(this);
        mStatusView = new TextView(this);
        mStatusView.setTextSize(18);
        mStatusView.setTextColor(0xFFFFFFFF);
        mStatusView.setBackgroundColor(0x80000000);
        mStatusView.setText("starting...");
        root.addView(mTextureView);
        root.addView(mStatusView);
        setContentView(root);

        mCameraThread = new HandlerThread("VCamViewerCamera");
        mCameraThread.start();
        mCameraHandler = new Handler(mCameraThread.getLooper());
        mCameraManager = getSystemService(CameraManager.class);

        mTextureView.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override
            public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
                mSurfaceReady = true;
                start();
            }
            @Override
            public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) {}
            @Override
            public boolean onSurfaceTextureDestroyed(SurfaceTexture st) { return true; }
            @Override
            public void onSurfaceTextureUpdated(SurfaceTexture st) {}
        });
    }

    private void start() {
        if (checkSelfPermission(Manifest.permission.CAMERA)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] { Manifest.permission.CAMERA }, REQ_CAMERA);
            return;
        }
        // Registration delivers the current availability snapshot, so if a
        // producer is already registered we get onCameraAvailable immediately.
        mCameraManager.registerAvailabilityCallback(mAvailability, mCameraHandler);
        status("waiting for virtual camera (start a producer)...");
        Log.i(TAG, "Watching availability of camera " + VIRTUAL_CAMERA_ID);
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        if (code == REQ_CAMERA && results.length > 0
                && results[0] == PackageManager.PERMISSION_GRANTED) {
            start();
        } else {
            status("CAMERA permission denied");
        }
    }

    private synchronized void maybeOpenCamera() {
        if (mOpening || mCamera != null || !mSurfaceReady) return;
        mOpening = true;
        try {
            CameraCharacteristics cc =
                    mCameraManager.getCameraCharacteristics(VIRTUAL_CAMERA_ID);
            StreamConfigurationMap map =
                    cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size size = map.getOutputSizes(SurfaceTexture.class)[0];
            Log.i(TAG, "Opening camera " + VIRTUAL_CAMERA_ID + " preview " + size);
            status("virtual camera appeared - opening @ " + size);

            SurfaceTexture st = mTextureView.getSurfaceTexture();
            st.setDefaultBufferSize(size.getWidth(), size.getHeight());
            Surface previewSurface = new Surface(st);

            mCameraManager.openCamera(VIRTUAL_CAMERA_ID,
                    new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice camera) {
                    Log.i(TAG, "Camera opened: " + camera.getId());
                    mCamera = camera;
                    mOpening = false;
                    createSession(camera, previewSurface);
                }
                @Override
                public void onDisconnected(CameraDevice camera) {
                    // Producer unregistered -> device removed while open.
                    Log.w(TAG, "Camera disconnected (producer gone)");
                    onCameraGone(camera);
                }
                @Override
                public void onError(CameraDevice camera, int error) {
                    Log.e(TAG, "Camera error " + error);
                    onCameraGone(camera);
                }
            }, mCameraHandler);
        } catch (Exception e) {
            // Race: camera vanished between availability and open.
            Log.w(TAG, "open failed (camera gone?): " + e.getMessage());
            mOpening = false;
            status("waiting for virtual camera (start a producer)...");
        }
    }

    private synchronized void onCameraGone(CameraDevice camera) {
        mOpening = false;
        if (mSession != null) {
            try { mSession.close(); } catch (Exception ignored) {}
            mSession = null;
        }
        try { camera.close(); } catch (Exception ignored) {}
        mCamera = null;
        mFrameCount = 0;
        status("waiting for virtual camera (start a producer)...");
    }

    private void createSession(CameraDevice camera, Surface previewSurface) {
        try {
            camera.createCaptureSession(Arrays.asList(previewSurface),
                    new CameraCaptureSession.StateCallback() {
                        @Override
                        public void onConfigured(CameraCaptureSession session) {
                            mSession = session;
                            try {
                                CaptureRequest.Builder b = camera.createCaptureRequest(
                                        CameraDevice.TEMPLATE_PREVIEW);
                                b.addTarget(previewSurface);
                                session.setRepeatingRequest(b.build(),
                                        new CameraCaptureSession.CaptureCallback() {
                                            @Override
                                            public void onCaptureCompleted(
                                                    CameraCaptureSession s,
                                                    CaptureRequest req,
                                                    TotalCaptureResult result) {
                                                mFrameCount++;
                                                if (mFrameCount % 30 == 0) {
                                                    Log.i(TAG, "RECEIVED " + mFrameCount
                                                            + " frames from camera "
                                                            + camera.getId());
                                                    status("frames: " + mFrameCount);
                                                }
                                            }
                                        }, mCameraHandler);
                                Log.i(TAG, "Repeating preview request started");
                                status("previewing...");
                            } catch (Exception e) {
                                Log.e(TAG, "setRepeatingRequest failed", e);
                            }
                        }
                        @Override
                        public void onConfigureFailed(CameraCaptureSession session) {
                            Log.e(TAG, "Session configuration failed");
                            status("session config failed");
                        }
                    }, mCameraHandler);
        } catch (Exception e) {
            Log.e(TAG, "createSession failed", e);
        }
    }

    private void status(String s) {
        runOnUiThread(() -> mStatusView.setText(s));
    }

    @Override
    protected void onDestroy() {
        if (mCameraManager != null) {
            mCameraManager.unregisterAvailabilityCallback(mAvailability);
        }
        if (mSession != null) mSession.close();
        if (mCamera != null) mCamera.close();
        mCameraThread.quitSafely();
        super.onDestroy();
    }
}
