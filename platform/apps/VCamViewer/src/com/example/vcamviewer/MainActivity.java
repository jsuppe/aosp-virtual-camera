/*
 * VCamViewer - Test scenario step 2.
 *
 * Activity that uses the camera permission to open the virtual camera
 * (id "100") via the standard Camera2 API and renders its stream to a
 * TextureView surface. Logs capture progress so the end-to-end flow can be
 * validated from logcat.
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
import java.util.List;

public class MainActivity extends Activity {
    private static final String TAG = "VCamViewer";
    private static final String PREFERRED_CAMERA_ID = "100";
    private static final int REQ_CAMERA = 1;

    private TextureView mTextureView;
    private TextView mStatusView;
    private HandlerThread mCameraThread;
    private Handler mCameraHandler;
    private CameraDevice mCamera;
    private CameraCaptureSession mSession;
    private long mFrameCount = 0;

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

        mTextureView.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override
            public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
                maybeOpenCamera();
            }
            @Override
            public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) {}
            @Override
            public boolean onSurfaceTextureDestroyed(SurfaceTexture st) { return true; }
            @Override
            public void onSurfaceTextureUpdated(SurfaceTexture st) {}
        });
    }

    private void maybeOpenCamera() {
        if (checkSelfPermission(Manifest.permission.CAMERA)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] { Manifest.permission.CAMERA }, REQ_CAMERA);
            return;
        }
        openCamera();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        if (code == REQ_CAMERA && results.length > 0
                && results[0] == PackageManager.PERMISSION_GRANTED) {
            openCamera();
        } else {
            status("CAMERA permission denied");
        }
    }

    private void openCamera() {
        try {
            CameraManager cm = getSystemService(CameraManager.class);
            List<String> ids = Arrays.asList(cm.getCameraIdList());
            Log.i(TAG, "Camera ids: " + ids);
            String target = ids.contains(PREFERRED_CAMERA_ID)
                    ? PREFERRED_CAMERA_ID
                    : (ids.isEmpty() ? null : ids.get(ids.size() - 1));
            if (target == null) {
                status("no cameras found");
                return;
            }

            CameraCharacteristics cc = cm.getCameraCharacteristics(target);
            StreamConfigurationMap map =
                    cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size size = map.getOutputSizes(SurfaceTexture.class)[0];
            Log.i(TAG, "Opening camera " + target + " preview " + size);
            status("opening camera " + target + " @ " + size);

            SurfaceTexture st = mTextureView.getSurfaceTexture();
            st.setDefaultBufferSize(size.getWidth(), size.getHeight());
            Surface previewSurface = new Surface(st);

            cm.openCamera(target, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice camera) {
                    Log.i(TAG, "Camera opened: " + camera.getId());
                    mCamera = camera;
                    createSession(camera, previewSurface);
                }
                @Override
                public void onDisconnected(CameraDevice camera) {
                    Log.w(TAG, "Camera disconnected");
                    camera.close();
                }
                @Override
                public void onError(CameraDevice camera, int error) {
                    Log.e(TAG, "Camera error " + error);
                    status("camera error " + error);
                    camera.close();
                }
            }, mCameraHandler);
        } catch (Exception e) {
            Log.e(TAG, "openCamera failed", e);
            status("openCamera failed: " + e.getMessage());
        }
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
        if (mSession != null) mSession.close();
        if (mCamera != null) mCamera.close();
        mCameraThread.quitSafely();
        super.onDestroy();
    }
}
