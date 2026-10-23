package com.example.vcamproducer;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.*;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.Range;
import android.view.*;
import android.widget.*;
import java.util.Collections;

/** Same producer frame: direct EGL mirror versus the real Camera2 path. */
public final class SplitDemoActivity extends Activity {
    private final Handler mUi = new Handler(Looper.getMainLooper());
    private TextureView mDirect, mPreview;
    private Surface mDirectSurface, mCameraSurface;
    private CameraManager mManager;
    private CameraDevice mCamera;
    private CameraCaptureSession mSession;
    private TextView mStatus;
    private FrameLayout mComparison;
    private ImageView mFrozen;
    private Button mFreeze;
    private Bitmap mFrozenBitmap;
    private boolean mActive, mWatching, mOpening, mCopyPending;
    private long mEpoch, mResults;
    private final long[] mTextureTimestamps = new long[2];
    private final int[] mTextureUpdates = new int[2];
    private String mCameraId;

    private final CameraManager.AvailabilityCallback mAvailability =
            new CameraManager.AvailabilityCallback() {
        @Override public void onCameraAvailable(String id) {
            if (mCameraId.equals(id)) maybeOpen();
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (getActionBar() != null) getActionBar().hide();
        mCameraId = getIntent().getStringExtra("camera");
        if (mCameraId == null) mCameraId = "100";
        mManager = getSystemService(CameraManager.class);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout root = new LinearLayout(this);
        root.setOnApplyWindowInsetsListener((view, insets) -> {
            android.graphics.Insets bars = insets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            return insets;
        });
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xff10141d);
        LinearLayout toolbar = new LinearLayout(this);
        toolbar.setPadding(16, 8, 16, 8);
        TextView title = label("VCam • split comparison\n1-second cycle • 60 frames • 0.017 → 1.000 seconds", 18);
        toolbar.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        mFreeze = new Button(this);
        mFreeze.setText("Freeze comparison");
        mFreeze.setOnClickListener(v -> toggleFreeze());
        toolbar.addView(mFreeze);
        root.addView(toolbar);
        mComparison = new FrameLayout(this);
        LinearLayout row = new LinearLayout(this);
        mDirect = pane(row, "LEFT • PRODUCER DIRECT\nSame generated frame, no camera path");
        mPreview = pane(row, "RIGHT • VIRTUAL CAMERA " + mCameraId + "\nProducer → HAL → Camera2");
        mComparison.addView(row, new FrameLayout.LayoutParams(-1, -1));
        mFrozen = new ImageView(this);
        mFrozen.setScaleType(ImageView.ScaleType.FIT_XY);
        mFrozen.setVisibility(View.GONE);
        mComparison.addView(mFrozen, new FrameLayout.LayoutParams(-1, -1));
        root.addView(mComparison, new LinearLayout.LayoutParams(-1, 0, 1));
        mStatus = label("Waiting for both surfaces…", 16);
        root.addView(mStatus);
        root.addView(label("Clock = cycle seconds.milliseconds • FRAME = 01–60 • Strip = 60 steps. At 60fps each step is ≈16.7ms; not a precision latency meter.", 15));
        setContentView(root);
        mDirect.setSurfaceTextureListener(listener(true));
        mPreview.setSurfaceTextureListener(listener(false));
    }

    private TextView label(String text, int size) {
        TextView v = new TextView(this);
        v.setText(text); v.setTextColor(0xffe7edf8); v.setTextSize(size);
        v.setPadding(16, 8, 16, 8);
        return v;
    }

    private TextureView pane(LinearLayout row, String heading) {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.addView(label(heading, 18));
        FrameLayout holder = new FrameLayout(this);
        TextureView texture = new TextureView(this) {
            @Override protected void onMeasure(int w, int h) {
                int width = MeasureSpec.getSize(w), height = MeasureSpec.getSize(h);
                if (width * 3 > height * 4) width = height * 4 / 3;
                else height = width * 3 / 4;
                setMeasuredDimension(width, height);
            }
        };
        holder.addView(texture, new FrameLayout.LayoutParams(-1, -1, Gravity.CENTER));
        column.addView(holder, new LinearLayout.LayoutParams(-1, 0, 1));
        row.addView(column, new LinearLayout.LayoutParams(0, -1, 1));
        return texture;
    }

    private TextureView.SurfaceTextureListener listener(boolean direct) {
        return new TextureView.SurfaceTextureListener() {
            @Override public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
                mTextureTimestamps[direct ? 0 : 1] = 0;
                st.setDefaultBufferSize(640, 480);
                if (direct) {
                    mDirectSurface = new Surface(st);
                    if (mActive) VCamProducerService.sReferenceSurface = mDirectSurface;
                }
                maybeOpen();
            }
            @Override public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) {}
            @Override public void onSurfaceTextureUpdated(SurfaceTexture st) {
                int side = direct ? 0 : 1;
                long timestamp = st.getTimestamp();
                if (timestamp > 0 && timestamp < mTextureTimestamps[side])
                    Log.w("VCamSplit", "TEXTURE_TIMESTAMP_BACKWARD side=" + side
                            + " previous=" + mTextureTimestamps[side] + " current=" + timestamp);
                mTextureTimestamps[side] = timestamp;
                if (++mTextureUpdates[side] % 120 == 0)
                    Log.i("VCamSplit", "TEXTURE_UPDATES side=" + side + " count="
                            + mTextureUpdates[side] + " timestamp=" + timestamp);
            }
            @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture st) {
                closeCamera();
                if (direct) {
                    if (VCamProducerService.sReferenceSurface == mDirectSurface)
                        VCamProducerService.sReferenceSurface = null;
                    if (mDirectSurface != null) mDirectSurface.release();
                    mDirectSurface = null;
                }
                return true;
            }
        };
    }

    @Override protected void onResume() {
        super.onResume();
        mActive = true;
        VCamProducerService.sReferenceSurface = mDirectSurface;
        startForegroundService(new Intent(this, VCamProducerService.class));
        watch();
    }

    private void watch() {
        if (!mActive) return;
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] { Manifest.permission.CAMERA }, 11);
            return;
        }
        if (!mWatching) {
            mWatching = true;
            mManager.registerAvailabilityCallback(mAvailability, mUi);
        }
        maybeOpen();
    }

    @Override public void onRequestPermissionsResult(int code, String[] permissions, int[] grants) {
        super.onRequestPermissionsResult(code, permissions, grants);
        if (code == 11 && grants.length > 0 && grants[0] == PackageManager.PERMISSION_GRANTED) watch();
        else mStatus.setText("Camera permission is needed to show the virtual camera.");
    }

    private void maybeOpen() {
        if (!mActive || !mWatching || mOpening || mCamera != null ||
                mDirectSurface == null || !mPreview.isAvailable()) return;
        mOpening = true;
        final long epoch = mEpoch;
        mStatus.setText("Opening virtual camera " + mCameraId + "…");
        try {
            mManager.openCamera(mCameraId, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice camera) {
                    if (!mActive || epoch != mEpoch) { camera.close(); return; }
                    mOpening = false;
                    mCamera = camera;
                    createSession(camera, epoch);
                }
                @Override public void onDisconnected(CameraDevice camera) {
                    camera.close();
                    if (epoch == mEpoch) { closeCamera(); mStatus.setText("Waiting for producer…"); }
                }
                @Override public void onError(CameraDevice camera, int error) {
                    camera.close();
                    if (epoch == mEpoch) { closeCamera(); mStatus.setText("Camera error " + error); }
                }
            }, mUi);
        } catch (Exception error) {
            mOpening = false;
            mStatus.setText("Waiting for virtual camera " + mCameraId + "…");
            Log.w("VCamSplit", "Camera not available yet", error);
        }
    }

    private void createSession(CameraDevice camera, long epoch) {
        try {
            SurfaceTexture st = mPreview.getSurfaceTexture();
            st.setDefaultBufferSize(640, 480);
            final Surface target = new Surface(st);
            mCameraSurface = target;
            camera.createCaptureSession(Collections.singletonList(target),
                    new CameraCaptureSession.StateCallback() {
                @Override public void onConfigured(CameraCaptureSession session) {
                    if (!mActive || epoch != mEpoch) { session.close(); return; }
                    mSession = session;
                    try {
                        CaptureRequest.Builder request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                        request.addTarget(target);
                        request.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, new Range<>(60, 60));
                        session.setRepeatingRequest(request.build(), new CameraCaptureSession.CaptureCallback() {
                            @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r, TotalCaptureResult result) {
                                if (epoch != mEpoch || !mActive) return;
                                if (++mResults % 30 == 0) {
                                    mStatus.setText("Live • camera " + mCameraId + " • " + mResults + " Camera2 results • Freeze to compare clock digits");
                                    Log.i("VCamSplit", "CAMERA2_RESULTS " + mResults + " camera=" + mCameraId);
                                }
                            }
                        }, mUi);
                    } catch (Exception error) {
                        mStatus.setText("Preview failed: " + error.getMessage());
                        Log.e("VCamSplit", "Preview failed", error);
                        closeCamera();
                    }
                }
                @Override public void onConfigureFailed(CameraCaptureSession session) {
                    session.close();
                    if (epoch == mEpoch) { closeCamera(); mStatus.setText("Camera session configuration failed"); }
                }
            }, mUi);
        } catch (Exception error) {
            mStatus.setText("Session failed: " + error.getMessage());
            Log.e("VCamSplit", "Session failed", error);
            closeCamera();
        }
    }

    private void closeCamera() {
        ++mEpoch;
        mOpening = false;
        if (mSession != null) { mSession.close(); mSession = null; }
        if (mCamera != null) { mCamera.close(); mCamera = null; }
        if (mCameraSurface != null) { mCameraSurface.release(); mCameraSurface = null; }
        mResults = 0;
    }

    private void toggleFreeze() {
        if (mFrozen.getVisibility() == View.VISIBLE) {
            mFrozen.setVisibility(View.GONE);
            mFrozen.setImageDrawable(null);
            mFrozenBitmap = null;
            mFreeze.setText("Freeze comparison");
            return;
        }
        if (mCopyPending || !mDirect.isAvailable() || !mPreview.isAvailable()) return;
        int[] pos = new int[2];
        mComparison.getLocationInWindow(pos);
        Rect area = new Rect(pos[0], pos[1], pos[0] + mComparison.getWidth(), pos[1] + mComparison.getHeight());
        Bitmap bitmap = Bitmap.createBitmap(area.width(), area.height(), Bitmap.Config.ARGB_8888);
        final long epoch = mEpoch;
        mCopyPending = true;
        // One composited-window snapshot, not two separately timed getBitmap calls.
        PixelCopy.request(getWindow(), area, bitmap, result -> {
            mCopyPending = false;
            if (result == PixelCopy.SUCCESS && mActive && epoch == mEpoch) {
                mFrozenBitmap = bitmap;
                mFrozen.setImageBitmap(bitmap);
                mFrozen.setVisibility(View.VISIBLE);
                mFreeze.setText("Resume live");
                Log.i("VCamSplit", "Comparison frozen from one window snapshot");
            } else {
                bitmap.recycle();
                Log.w("VCamSplit", "Snapshot not displayed, status=" + result);
            }
        }, mUi);
    }

    @Override protected void onPause() {
        mActive = false;
        if (mWatching) { mManager.unregisterAvailabilityCallback(mAvailability); mWatching = false; }
        if (VCamProducerService.sReferenceSurface == mDirectSurface)
            VCamProducerService.sReferenceSurface = null;
        closeCamera();
        mFrozen.setVisibility(View.GONE);
        mFrozen.setImageDrawable(null);
        mFrozenBitmap = null;
        mFreeze.setText("Freeze comparison");
        super.onPause();
    }
}
