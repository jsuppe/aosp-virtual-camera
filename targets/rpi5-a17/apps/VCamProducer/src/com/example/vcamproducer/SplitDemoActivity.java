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
    private SurfaceView mSurfacePreview;
    private boolean mUseSurfaceView, mSurfaceReady;
    private Surface mDirectSurface, mCameraSurface;
    private CameraManager mManager;
    private CameraDevice mCamera;
    private CameraCaptureSession mSession;
    private TextView mStatus;
    private FrameLayout mComparison;
    private ImageView mFrozen;
    private Button mFreeze;
    private Bitmap mFrozenBitmap;
    private boolean mActive, mWatching, mOpening, mCopyPending, mCameraFault;
    private long mEpoch, mResults;
    private final long[] mTextureTimestamps = new long[2];
    private final int[] mTextureUpdates = new int[2];
    private String mCameraId;
    private int mTargetFps;
    private int mStreamWidth, mStreamHeight;
    private FrameTrace mCaptureTrace;
    private boolean mProfiling;
    private static final CaptureResult.Key<Long> PRODUCER_TIMESTAMP = new CaptureResult.Key<>(
            "com.virtualcamera.producerTimestampNs",Long.class);

    private final CameraManager.AvailabilityCallback mAvailability =
            new CameraManager.AvailabilityCallback() {
        @Override public void onCameraAvailable(String id) {
            if (mCameraId.equals(id)) maybeOpen();
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (getActionBar() != null) getActionBar().hide();
        int requestedFps = state == null ? getIntent().getIntExtra("fps", 60)
                : state.getInt("selected_fps", 60);
        mTargetFps = requestedFps == 30 || requestedFps == 120 ? requestedFps : 60;
        int requestedWidth = state == null ? getIntent().getIntExtra("width", 640)
                : state.getInt("selected_width", 640);
        mStreamWidth = requestedWidth == 3840 ? 3840 : requestedWidth == 1920 ? 1920 : 640;
        mStreamHeight = mStreamWidth == 3840 ? 2160 : mStreamWidth == 1920 ? 1080 : 480;
        // The default producer advertises UHD at up to 30 fps only.
        if (mStreamWidth == 3840) mTargetFps = 30;
        getIntent().putExtra("width", mStreamWidth).putExtra("fps", mTargetFps);
        mUseSurfaceView=state==null ? getIntent().getBooleanExtra("surface_preview",true)
                : state.getBoolean("selected_surface_preview",true);
        getIntent().putExtra("surface_preview",mUseSurfaceView);
        int profileRun=getIntent().getIntExtra("profile_run",0);
        mProfiling=profileRun>0;
        mCaptureTrace=new FrameTrace(new java.io.File(getFilesDir(),"vcam-profile"),
                "camera-"+profileRun+"-"+System.nanoTime(),
                "callback_ns,frame,status,sensor_ns,producer_ns,duration_ns,target_fps,epoch",profileRun>0);
        WindowManager.LayoutParams windowParams = getWindow().getAttributes();
        windowParams.preferredRefreshRate = mTargetFps;
        getWindow().setAttributes(windowParams);
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
        TextView title = label("VCam • split comparison • " + mStreamWidth + "×" + mStreamHeight
                + "\n1-second cycle • " + mTargetFps
                + " frames • " + String.format(java.util.Locale.ROOT, "%.3f", 1.0 / mTargetFps)
                + " → 1.000 seconds", 18);
        toolbar.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        Button resolution = new Button(this);
        resolution.setText(mStreamWidth == 3840 ? "Resolution: 4K UHD" : mStreamWidth == 1920 ? "Resolution: 1080p" : "Resolution: VGA");
        resolution.setOnClickListener(v -> {
            PopupMenu menu = new PopupMenu(this, resolution);
            menu.getMenu().add(0, 640, 0, "640×480 (VGA)");
            menu.getMenu().add(0, 1920, 1, "1920×1080 (1080p)");
            menu.getMenu().add(0, 3840, 2, "3840×2160 (4K UHD, 30 fps target)");
            menu.setOnMenuItemClickListener(item -> {
                switchMode(item.getItemId(), mTargetFps);
                return true;
            });
            menu.show();
        });
        toolbar.addView(resolution);
        Button rate = new Button(this);
        rate.setText("Frame rate: " + mTargetFps);
        rate.setOnClickListener(v -> {
            PopupMenu menu = new PopupMenu(this, rate);
            for (int fps : (mStreamWidth == 3840 ? new int[] {30} : new int[] {30, 60, 120})) menu.getMenu().add(0, fps, fps, fps + " fps target");
            menu.setOnMenuItemClickListener(item -> {
                switchMode(mStreamWidth, item.getItemId());
                return true;
            });
            menu.show();
        });
        toolbar.addView(rate);
        Button previewKind=new Button(this);
        previewKind.setText(mUseSurfaceView ? "Preview: Surface" : "Preview: Texture");
        previewKind.setOnClickListener(v -> {
            getIntent().putExtra("surface_preview",!mUseSurfaceView);
            recreate();
        });
        toolbar.addView(previewKind);
        mFreeze = new Button(this);
        mFreeze.setText("Freeze comparison");
        mFreeze.setOnClickListener(v -> toggleFreeze());
        toolbar.addView(mFreeze);
        root.addView(toolbar);
        mComparison = new FrameLayout(this);
        LinearLayout row = new LinearLayout(this);
        mDirect = pane(row, "LEFT • PRODUCER DIRECT\nSame generated frame, no camera path");
        mDirect.setContentDescription("Tap to change the producer background color");
        mDirect.setOnClickListener(v -> {
            if (!mActive || mCopyPending || mFrozen.getVisibility() == View.VISIBLE) return;
            VCamProducerService.changeBackground();
        });
        String cameraHeading="RIGHT • VIRTUAL CAMERA " + mCameraId + "\nProducer → HAL → Camera2";
        if(mUseSurfaceView) createSurfacePane(row,cameraHeading);
        else mPreview = pane(row,cameraHeading);
        mComparison.addView(row, new FrameLayout.LayoutParams(-1, -1));
        mFrozen = new ImageView(this);
        mFrozen.setScaleType(ImageView.ScaleType.FIT_XY);
        mFrozen.setVisibility(View.GONE);
        mComparison.addView(mFrozen, new FrameLayout.LayoutParams(-1, -1));
        root.addView(mComparison, new LinearLayout.LayoutParams(-1, 0, 1));
        mStatus = label("Waiting for both surfaces…", 16);
        root.addView(mStatus);
        root.addView(label("Clock = nominal cycle seconds • " + mTargetFps + " steps • ≈"
                + String.format(java.util.Locale.ROOT, "%.2f", 1000.0 / mTargetFps)
                + "ms per step at target rate; not a precision latency meter.", 15));
        setContentView(root);
        mDirect.setSurfaceTextureListener(listener(true));
        if(mPreview!=null) mPreview.setSurfaceTextureListener(listener(false));
    }

    private TextView label(String text, int size) {
        TextView v = new TextView(this);
        v.setText(text); v.setTextColor(0xffe7edf8); v.setTextSize(size);
        v.setPadding(16, 8, 16, 8);
        return v;
    }

    private void switchMode(int width, int fps) {
        if (!mActive || (width == mStreamWidth && fps == mTargetFps)) return;
        // Recreate only the preview/activity. Keep the registered producer alive;
        // Camera2 renegotiates the stream and the graphics owner retires old EGL.
        Intent intent = new Intent(getIntent());
        intent.putExtra("width", width).putExtra("fps", fps);
        setIntent(intent);
        recreate();
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        state.putInt("selected_width", getIntent().getIntExtra("width", mStreamWidth));
        state.putInt("selected_fps", getIntent().getIntExtra("fps", mTargetFps));
        state.putBoolean("selected_surface_preview",getIntent().getBooleanExtra("surface_preview",true));
        super.onSaveInstanceState(state);
    }

    private TextureView pane(LinearLayout row, String heading) {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.addView(label(heading, 18));
        FrameLayout holder = new FrameLayout(this);
        TextureView texture = new TextureView(this) {
            @Override protected void onMeasure(int w, int h) {
                int width = MeasureSpec.getSize(w), height = MeasureSpec.getSize(h);
                if ((long) width * mStreamHeight > (long) height * mStreamWidth)
                    width = height * mStreamWidth / mStreamHeight;
                else height = width * mStreamHeight / mStreamWidth;
                setMeasuredDimension(width, height);
            }
        };
        holder.addView(texture, new FrameLayout.LayoutParams(-1, -1, Gravity.CENTER));
        column.addView(holder, new LinearLayout.LayoutParams(-1, 0, 1));
        row.addView(column, new LinearLayout.LayoutParams(0, -1, 1));
        return texture;
    }

    private void createSurfacePane(LinearLayout row,String heading) {
        LinearLayout column=new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.addView(label(heading,18));
        FrameLayout holder=new FrameLayout(this);
        mSurfacePreview=new SurfaceView(this) {
            @Override protected void onMeasure(int w,int h) {
                int width=MeasureSpec.getSize(w),height=MeasureSpec.getSize(h);
                if((long)width*mStreamHeight>(long)height*mStreamWidth)
                    width=height*mStreamWidth/mStreamHeight;
                else height=width*mStreamHeight/mStreamWidth;
                setMeasuredDimension(width,height);
            }
        };
        holder.addView(mSurfacePreview,new FrameLayout.LayoutParams(-1,-1,Gravity.CENTER));
        column.addView(holder,new LinearLayout.LayoutParams(-1,0,1));
        row.addView(column,new LinearLayout.LayoutParams(0,-1,1));
        SurfaceHolder surfaceHolder=mSurfacePreview.getHolder();
        surfaceHolder.setFormat(android.graphics.PixelFormat.RGBA_8888);
        surfaceHolder.setFixedSize(mStreamWidth,mStreamHeight);
        surfaceHolder.addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) {}
            @Override public void surfaceChanged(SurfaceHolder h,int format,int w,int height) {
                boolean ready=h.getSurface().isValid() && w==mStreamWidth && height==mStreamHeight;
                if(mSurfaceReady && !ready) closeCamera();
                mSurfaceReady=ready;
                if(ready) {
                    h.getSurface().setFrameRate(mTargetFps,Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
                    Log.i("VCamSplit","SURFACE_PREVIEW_READY "+w+"x"+height);
                    maybeOpen();
                }
            }
            @Override public void surfaceDestroyed(SurfaceHolder h) {
                mSurfaceReady=false;
                closeCamera();
            }
        });
    }

    private boolean cameraPreviewReady() {
        return mUseSurfaceView ? mSurfaceReady : mPreview!=null && mPreview.isAvailable();
    }

    private TextureView.SurfaceTextureListener listener(boolean direct) {
        return new TextureView.SurfaceTextureListener() {
            @Override public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
                mTextureTimestamps[direct ? 0 : 1] = 0;
                st.setDefaultBufferSize(mStreamWidth, mStreamHeight);
                if (direct) {
                    mDirectSurface = new Surface(st);
                    mDirectSurface.setFrameRate(mTargetFps, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
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
        startForegroundService(new Intent(this, VCamProducerService.class)
                .putExtra("profile_run",getIntent().getIntExtra("profile_run",0))
                .putExtra("minimal_scene",getIntent().getBooleanExtra("minimal_scene",false))
                .putExtra("mirror",getIntent().getBooleanExtra("mirror",true))
                .putExtra("clock",getIntent().getBooleanExtra("clock",true)));
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
        if (!mActive || !mWatching || mOpening || mCameraFault || mCamera != null ||
                mDirectSurface == null || !cameraPreviewReady()) return;
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
                    if (epoch == mEpoch) {
                        mCameraFault = true; // do not loop on an invalid configuration
                        Log.e("VCamSplit", "Camera error " + error);
                        closeCamera(); mStatus.setText("Camera error " + error + " • select a mode to retry");
                    }
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
            final Surface target;
            if(mUseSurfaceView) {
                if(!mSurfaceReady) return;
                target=mSurfacePreview.getHolder().getSurface(); // borrowed; holder owns lifetime
            } else {
                SurfaceTexture st=mPreview.getSurfaceTexture();
                st.setDefaultBufferSize(mStreamWidth,mStreamHeight);
                target=new Surface(st);
            }
            target.setFrameRate(mTargetFps, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
            mCameraSurface = target;
            camera.createCaptureSession(Collections.singletonList(target),
                    new CameraCaptureSession.StateCallback() {
                @Override public void onConfigured(CameraCaptureSession session) {
                    if (!mActive || epoch != mEpoch) { session.close(); return; }
                    mSession = session;
                    try {
                        Range<Integer> selectedRate = new Range<>(mTargetFps, mTargetFps);
                        Range<Integer>[] advertised = mManager.getCameraCharacteristics(mCameraId)
                                .get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
                        if (advertised == null || !java.util.Arrays.asList(advertised).contains(selectedRate))
                            throw new IllegalArgumentException("Target rate is not advertised: " + selectedRate);
                        CaptureRequest.Builder request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                        request.addTarget(target);
                        request.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, selectedRate);
                        Log.i("VCamSplit", "REQUESTED_FPS " + mTargetFps);
                        session.setRepeatingRequest(request.build(), new CameraCaptureSession.CaptureCallback() {
                            @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r, TotalCaptureResult result) {
                                long received=mProfiling?System.nanoTime():0;
                                if (epoch != mEpoch || !mActive) return;
                                if (mProfiling) {
                                Long sensor=result.get(CaptureResult.SENSOR_TIMESTAMP);
                                Long duration=result.get(CaptureResult.SENSOR_FRAME_DURATION);
                                Long producer=result.get(PRODUCER_TIMESTAMP);
                                mCaptureTrace.add(received,result.getFrameNumber(),0,
                                        sensor==null?0:sensor,producer==null?0:producer,
                                        duration==null?0:duration,mTargetFps,epoch);
                                }
                                if (++mResults % 30 == 0) {
                                    mStatus.setText("Live • camera " + mCameraId + " • " + mResults + " Camera2 results • Freeze to compare clock digits");
                                    Log.i("VCamSplit", "CAMERA2_RESULTS " + mResults + " camera=" + mCameraId);
                                }
                            }
                            @Override public void onCaptureFailed(CameraCaptureSession s,CaptureRequest r,CaptureFailure failure) {
                                if(mProfiling && epoch==mEpoch && mActive) mCaptureTrace.add(System.nanoTime(),
                                        failure.getFrameNumber(),failure.getReason()+1,0,0,0,mTargetFps,epoch);
                            }
                        }, mUi);
                    } catch (Exception error) {
                        mCameraFault = true;
                        mStatus.setText("Preview failed: " + error.getMessage());
                        Log.e("VCamSplit", "Preview failed", error);
                        closeCamera();
                    }
                }
                @Override public void onConfigureFailed(CameraCaptureSession session) {
                    session.close();
                    if (epoch == mEpoch) {
                        mCameraFault = true;
                        closeCamera(); mStatus.setText("Camera session configuration failed • select a mode to retry");
                    }
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
        if (mCameraSurface != null) { if(!mUseSurfaceView) mCameraSurface.release(); mCameraSurface = null; }
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
        if (mCopyPending || !mDirect.isAvailable() || !cameraPreviewReady()) return;
        if(mUseSurfaceView) { freezeCompositedDisplay(); return; }
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

    private void freezeCompositedDisplay() {
        final long epoch=mEpoch;
        final int[] pos=new int[2];
        mComparison.getLocationOnScreen(pos);
        final int width=mComparison.getWidth(),height=mComparison.getHeight();
        mCopyPending=true;
        // One compositor capture includes the separate SurfaceView layer. Window
        // PixelCopy alone would omit it; two separate copies would not be simultaneous.
        android.window.ScreenCapture.capture(
                new android.window.ScreenCapture.ScreenCaptureParams.Builder(getDisplay().getDisplayId()).build(),
                getMainExecutor(),
                new android.os.OutcomeReceiver<android.window.ScreenCapture.ScreenCaptureResult,Exception>() {
            @Override public void onResult(android.window.ScreenCapture.ScreenCaptureResult result) {
                Bitmap full=null,hardware=null,cropped=null;
                try(android.hardware.HardwareBuffer buffer=result.getHardwareBuffer()) {
                    hardware=Bitmap.wrapHardwareBuffer(buffer,result.getColorSpace());
                    if(hardware==null) throw new IllegalStateException("Capture bitmap unavailable");
                    full=hardware.copy(Bitmap.Config.ARGB_8888,false);
                    if(full==null) throw new IllegalStateException("Capture copy unavailable");
                    cropped=Bitmap.createBitmap(full,pos[0],pos[1],width,height);
                    mCopyPending=false;
                    if(mActive && epoch==mEpoch) {
                        mFrozenBitmap=cropped;
                        mFrozen.setImageBitmap(cropped);
                        mFrozen.setVisibility(View.VISIBLE);
                        mFreeze.setText("Resume live");
                        Log.i("VCamSplit","Comparison frozen from one compositor snapshot");
                    } else cropped.recycle();
                } catch(Exception error) { onError(error); }
                finally {
                    if(hardware!=null) hardware.recycle();
                    if(full!=null && full!=cropped) full.recycle();
                }
            }
            @Override public void onError(Exception error) {
                mCopyPending=false;
                mStatus.setText("Could not freeze comparison");
                Log.e("VCamSplit","Compositor snapshot failed",error);
            }
        });
    }

    @Override protected void onPause() {
        mActive = false;
        if (mWatching) { mManager.unregisterAvailabilityCallback(mAvailability); mWatching = false; }
        if (VCamProducerService.sReferenceSurface == mDirectSurface)
            VCamProducerService.sReferenceSurface = null;
        closeCamera();
        try { mCaptureTrace.close(); } catch(java.io.IOException e) { Log.e("VCamSplit","Profile export",e); }
        mFrozen.setVisibility(View.GONE);
        mFrozen.setImageDrawable(null);
        mFrozenBitmap = null;
        mFreeze.setText("Freeze comparison");
        super.onPause();
    }
}
