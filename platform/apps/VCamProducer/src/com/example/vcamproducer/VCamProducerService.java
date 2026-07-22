/*
 * VCamProducerService - Test scenario step 1.
 *
 * Android Service that registers as a frame producer with the platform
 * VirtualCameraService ("virtual_camera" binder service) and produces frames
 * into the Surface delivered through IVirtualCameraCallback (the Surface is
 * the producer end of a BufferQueue owned by the virtual camera HAL).
 */
package com.example.vcamproducer;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.hardware.virtualcamera.IVirtualCameraCallback;
import android.hardware.virtualcamera.IVirtualCameraService;
import android.hardware.virtualcamera.StreamConfig;
import android.hardware.virtualcamera.VirtualCameraConfig;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.util.Log;
import android.view.Surface;

public class VCamProducerService extends Service {
    private static final String TAG = "VCamProducer";
    private static final String CHANNEL_ID = "vcam_producer";

    private IVirtualCameraService mService;
    private int mCameraId = -1;

    private HandlerThread mRegisterThread;
    private Handler mRegisterHandler;

    private final Object mRenderLock = new Object();
    private RenderThread mRenderThread;

    @Override
    public void onCreate() {
        super.onCreate();
        startForegroundWithNotification();

        mRegisterThread = new HandlerThread("VCamRegister");
        mRegisterThread.start();
        mRegisterHandler = new Handler(mRegisterThread.getLooper());
        mRegisterHandler.post(this::registerWithRetry);
    }

    private void startForegroundWithNotification() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(
                CHANNEL_ID, "VCam Producer", NotificationManager.IMPORTANCE_LOW));
        Notification n = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("VCam Producer")
                .setContentText("Producing virtual camera frames")
                .setSmallIcon(android.R.drawable.ic_menu_camera)
                .build();
        startForeground(1, n);
    }

    private void registerWithRetry() {
        if (mCameraId >= 0) return;
        try {
            IBinder binder = ServiceManager.getService("virtual_camera");
            if (binder == null) {
                Log.w(TAG, "virtual_camera service not up yet, retrying in 2s");
                mRegisterHandler.postDelayed(this::registerWithRetry, 2000);
                return;
            }
            mService = IVirtualCameraService.Stub.asInterface(binder);

            VirtualCameraConfig config = new VirtualCameraConfig();
            config.name = "VCamProducer";
            config.maxWidth = 1920;
            config.maxHeight = 1080;
            config.maxFps = 30;
            config.supportedFormats = new int[] { 1 /* RGBA_8888 */ };
            config.facing = 2;      // EXTERNAL
            config.orientation = 0;
            config.supportsJpeg = false;
            config.uniqueId = "vcam-producer-1";

            mCameraId = mService.registerCamera(config, mCallback);
            Log.i(TAG, "REGISTERED as producer, virtual camera id=" + mCameraId);
        } catch (Exception e) {
            Log.e(TAG, "register failed, retrying in 2s", e);
            mRegisterHandler.postDelayed(this::registerWithRetry, 2000);
        }
    }

    private final IVirtualCameraCallback.Stub mCallback = new IVirtualCameraCallback.Stub() {
        @Override
        public void onCameraOpened() {
            Log.i(TAG, "Callback: onCameraOpened");
        }

        @Override
        public void onStreamsConfigured(StreamConfig[] streams, Surface[] surfaces) {
            Log.i(TAG, "Callback: onStreamsConfigured, " + streams.length + " stream(s)");
            if (streams.length == 0 || surfaces.length == 0) return;
            StreamConfig s = streams[0];
            Log.i(TAG, "Stream 0: " + s.width + "x" + s.height + " fmt=" + s.format
                    + " fps=" + s.fps);
            synchronized (mRenderLock) {
                stopRenderLocked();
                mRenderThread = new RenderThread(surfaces[0], s.width, s.height,
                        s.fps > 0 ? s.fps : 30);
                mRenderThread.start();
            }
        }

        @Override
        public void onCaptureStarted(int frameRate) {
            Log.i(TAG, "Callback: onCaptureStarted @" + frameRate);
        }

        @Override
        public void onCaptureStopped() {
            Log.i(TAG, "Callback: onCaptureStopped");
        }

        @Override
        public void onCameraClosed() {
            Log.i(TAG, "Callback: onCameraClosed");
            synchronized (mRenderLock) {
                stopRenderLocked();
            }
        }

        @Override
        public void onStillCaptureRequested(Surface surface, int captureId) {
            Log.i(TAG, "Callback: onStillCaptureRequested " + captureId);
        }
    };

    private void stopRenderLocked() {
        if (mRenderThread != null) {
            mRenderThread.quit();
            mRenderThread = null;
        }
    }

    /** Draws an animated pattern into the Surface at the stream frame rate. */
    private static class RenderThread extends Thread {
        private final Surface mSurface;
        private final int mWidth, mHeight, mFps;
        private volatile boolean mRunning = true;
        private long mFrames = 0;

        RenderThread(Surface surface, int width, int height, int fps) {
            super("VCamRender");
            mSurface = surface;
            mWidth = width;
            mHeight = height;
            mFps = fps;
        }

        void quit() {
            mRunning = false;
            interrupt();
        }

        @Override
        public void run() {
            Log.i(TAG, "Render thread started " + mWidth + "x" + mHeight + "@" + mFps);
            Paint textPaint = new Paint();
            textPaint.setColor(Color.WHITE);
            textPaint.setTextSize(Math.max(24, mHeight / 12));
            textPaint.setAntiAlias(true);
            Paint ballPaint = new Paint();
            ballPaint.setColor(Color.WHITE);
            ballPaint.setAntiAlias(true);

            long frameIntervalMs = 1000L / mFps;
            while (mRunning && mSurface.isValid()) {
                long t0 = System.currentTimeMillis();
                try {
                    Canvas c = mSurface.lockCanvas(null);
                    try {
                        float hue = (mFrames % 360);
                        c.drawColor(Color.HSVToColor(new float[] { hue, 0.8f, 0.6f }));
                        // bouncing ball
                        float phase = (mFrames % 120) / 120f;
                        float x = mWidth * 0.1f + (mWidth * 0.8f) * phase;
                        float y = mHeight * 0.5f
                                + (float) (Math.sin(phase * 2 * Math.PI) * mHeight * 0.3f);
                        c.drawCircle(x, y, Math.max(12, mWidth / 24f), ballPaint);
                        c.drawText("AIDL frame " + mFrames, mWidth / 16f, mHeight / 6f,
                                textPaint);
                    } finally {
                        mSurface.unlockCanvasAndPost(c);
                    }
                    mFrames++;
                    if (mFrames % 30 == 0) {
                        Log.i(TAG, "PRODUCED " + mFrames + " frames");
                    }
                } catch (Exception e) {
                    Log.e(TAG, "render failed (surface gone?)", e);
                    break;
                }
                long elapsed = System.currentTimeMillis() - t0;
                long sleepMs = frameIntervalMs - elapsed;
                if (sleepMs > 2) {
                    try {
                        Thread.sleep(sleepMs);
                    } catch (InterruptedException e) {
                        // quitting
                    }
                }
            }
            Log.i(TAG, "Render thread exiting after " + mFrames + " frames");
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        synchronized (mRenderLock) {
            stopRenderLocked();
        }
        try {
            if (mService != null && mCameraId >= 0) {
                mService.unregisterCamera(mCameraId);
            }
        } catch (RemoteException e) {
            Log.e(TAG, "unregister failed", e);
        }
        mRegisterThread.quitSafely();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
