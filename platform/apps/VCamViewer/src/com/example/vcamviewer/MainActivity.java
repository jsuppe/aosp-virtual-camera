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
import android.hardware.camera2.CaptureResult;
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
    /** Vendor tag published by the virtual camera HAL (see hal/core/VendorTags.h). */
    private static final CaptureResult.Key<Long> KEY_PRODUCER_TS =
            new CaptureResult.Key<>("com.virtualcamera.producerTimestampNs", Long.class);
    private long mLatencySumNs = 0;
    private int mLatencyCount = 0;
    /** --ez yuv true: add a YUV_420_888 ImageReader target (exercises the HAL's YUV path). */
    private boolean mYuvMode = false;
    private android.media.ImageReader mYuvReader;
    private int mYuvFrames = 0;
    /** --ez jpeg true: add a JPEG ImageReader and take one still after the preview settles. */
    private boolean mJpegMode = false;
    private android.media.ImageReader mJpegReader;
    private boolean mStillTaken = false;
    /** --es camera 101 selects another virtual camera (default 100 = HAL slot 0). */
    private String VIRTUAL_CAMERA_ID = "100";
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
        String cam = getIntent().getStringExtra("camera");
        if (cam != null && !cam.isEmpty()) VIRTUAL_CAMERA_ID = cam;
        Log.i(TAG, "Viewer for virtual camera " + VIRTUAL_CAMERA_ID);

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

            mYuvMode = getIntent().getBooleanExtra("yuv", false);
            if (mYuvMode) {
                mYuvReader = android.media.ImageReader.newInstance(size.getWidth(), size.getHeight(),
                        android.graphics.ImageFormat.YUV_420_888, 3);
                mYuvReader.setOnImageAvailableListener(reader -> {
                    android.media.Image img = reader.acquireLatestImage();
                    if (img == null) return;
                    mYuvFrames++;
                    if (mYuvFrames == 1 || mYuvFrames % 90 == 0) {
                        android.media.Image.Plane[] p = img.getPlanes();
                        java.nio.ByteBuffer y = p[0].getBuffer();
                        java.nio.ByteBuffer u = p[1].getBuffer();
                        // Sample a few bytes so "non-black, plausible" is verifiable from logcat.
                        int mid = y.limit() / 2;
                        Log.i(TAG, "YUV frame " + mYuvFrames + " " + img.getWidth() + "x" + img.getHeight()
                                + " yStride=" + p[0].getRowStride() + " uvStride=" + p[1].getRowStride()
                                + " uvPixelStride=" + p[1].getPixelStride()
                                + " Y[mid]=" + (y.get(mid) & 0xff) + " U[0]=" + (u.get(0) & 0xff));
                    }
                    img.close();
                }, mCameraHandler);
                Log.i(TAG, "YUV mode: ImageReader " + size + " added as second target");
            }
            mJpegMode = getIntent().getBooleanExtra("jpeg", false);
            if (mJpegMode) {
                Size jsize = map.getOutputSizes(android.graphics.ImageFormat.JPEG)[0];
                mJpegReader = android.media.ImageReader.newInstance(jsize.getWidth(), jsize.getHeight(),
                        android.graphics.ImageFormat.JPEG, 2);
                mJpegReader.setOnImageAvailableListener(reader -> {
                    android.media.Image img = reader.acquireLatestImage();
                    if (img == null) return;
                    java.nio.ByteBuffer b = img.getPlanes()[0].getBuffer();
                    int n = b.remaining();
                    // ImageReader trims the BLOB to the size in the camera3_jpeg_blob
                    // trailer, so n is the JPEG length; check SOI/EOI markers.
                    boolean soi = n > 4 && (b.get(0) & 0xff) == 0xFF && (b.get(1) & 0xff) == 0xD8;
                    boolean eoi = n > 4 && (b.get(n - 2) & 0xff) == 0xFF && (b.get(n - 1) & 0xff) == 0xD9;
                    Log.i(TAG, "JPEG still " + img.getWidth() + "x" + img.getHeight() + ": " + n
                            + " bytes, SOI=" + soi + " EOI=" + eoi
                            + (soi && eoi ? " -> VALID JPEG" : " -> INVALID"));
                    status("JPEG still: " + n + " bytes " + (soi && eoi ? "valid" : "INVALID"));
                    img.close();
                }, mCameraHandler);
                Log.i(TAG, "JPEG mode: ImageReader " + jsize + " added; will capture one still");
            }

            mCameraManager.openCamera(VIRTUAL_CAMERA_ID,
                    new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice camera) {
                    Log.i(TAG, "Camera opened: " + camera.getId());
                    mCamera = camera;
                    mOpening = false;
                    createSession(camera, previewSurface);   // adds the YUV target if enabled
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
            java.util.List<Surface> targets = new java.util.ArrayList<>();
            targets.add(previewSurface);
            if (mYuvReader != null) targets.add(mYuvReader.getSurface());
            if (mJpegReader != null) targets.add(mJpegReader.getSurface());
            camera.createCaptureSession(targets,
                    new CameraCaptureSession.StateCallback() {
                        @Override
                        public void onConfigured(CameraCaptureSession session) {
                            mSession = session;
                            try {
                                CaptureRequest.Builder b = camera.createCaptureRequest(
                                        CameraDevice.TEMPLATE_PREVIEW);
                                b.addTarget(previewSurface);
                                if (mYuvReader != null) b.addTarget(mYuvReader.getSurface());
                                session.setRepeatingRequest(b.build(),
                                        new CameraCaptureSession.CaptureCallback() {
                                            @Override
                                            public void onCaptureCompleted(
                                                    CameraCaptureSession s,
                                                    CaptureRequest req,
                                                    TotalCaptureResult result) {
                                                mFrameCount++;
                                                // Producer->result latency: the HAL carries the
                                                // producer's BufferQueue timestamp (CLOCK_MONOTONIC,
                                                // same clock as System.nanoTime()) in a vendor tag.
                                                long now = System.nanoTime();
                                                Long producerTs = null;
                                                try {
                                                    producerTs = result.get(KEY_PRODUCER_TS);
                                                } catch (IllegalArgumentException ignored) {
                                                    // vendor tag not published by this HAL build
                                                }
                                                Long sensorTs = result.get(CaptureResult.SENSOR_TIMESTAMP);
                                                if (producerTs != null) {
                                                    mLatencySumNs += (now - producerTs);
                                                    mLatencyCount++;
                                                }
                                                if (mJpegReader != null && !mStillTaken && mFrameCount == 60) {
                                                    mStillTaken = true;
                                                    try {
                                                        CaptureRequest.Builder sb = camera.createCaptureRequest(
                                                                CameraDevice.TEMPLATE_STILL_CAPTURE);
                                                        sb.addTarget(mJpegReader.getSurface());
                                                        sb.set(CaptureRequest.JPEG_QUALITY, (byte) 90);
                                                        s.capture(sb.build(), null, mCameraHandler);
                                                        Log.i(TAG, "JPEG still requested (TEMPLATE_STILL_CAPTURE, q90)");
                                                    } catch (Exception e) {
                                                        Log.e(TAG, "still capture failed", e);
                                                    }
                                                }
                                                if (mFrameCount % 30 == 0) {
                                                    String lat = "";
                                                    if (mLatencyCount > 0) {
                                                        lat = String.format(" latency producer->result %.1f ms",
                                                                (mLatencySumNs / (double) mLatencyCount) / 1e6);
                                                        mLatencySumNs = 0; mLatencyCount = 0;
                                                    }
                                                    if (sensorTs != null) {
                                                        lat += String.format(" (slot->result %.1f ms)",
                                                                (now - sensorTs) / 1e6);
                                                    }
                                                    Log.i(TAG, "RECEIVED " + mFrameCount
                                                            + " frames from camera "
                                                            + camera.getId() + lat);
                                                    status("frames: " + mFrameCount + lat);
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
