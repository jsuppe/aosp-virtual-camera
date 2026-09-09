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
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
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
    /** --ei tint N: colour multiplier preset so two producers are told apart on screen. */
    private static volatile int sTint = 0;

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

    /**
     * Renders an animated scene into the Surface with OpenGL ES 2.0 (GPU).
     * The Surface is an ANativeWindow whose buffers EGL allocates with GPU
     * render-target usage, so no CPU touches the pixels on the producer side.
     * A single full-screen fragment shader draws a time-varying gradient plus
     * a moving disc — clearly GPU-generated, and RGBA so the HAL can consume
     * it with zero color conversion.
     */
    private static class RenderThread extends Thread {
        private final Surface mSurface;
        private final int mWidth, mHeight, mFps;
        private volatile boolean mRunning = true;
        private long mFrames = 0;

        private static final String VERT =
                "attribute vec2 aPos;\n" +
                "varying vec2 vUv;\n" +
                "void main() {\n" +
                "  vUv = aPos * 0.5 + 0.5;\n" +
                "  gl_Position = vec4(aPos, 0.0, 1.0);\n" +
                "}\n";
        // uBall/uBallR are in uv space; aspect corrects the disc to a circle.
        private static final String FRAG =
                "precision mediump float;\n" +
                "varying vec2 vUv;\n" +
                "uniform float uTime;\n" +
                "uniform vec2 uBall;\n" +
                "uniform float uBallR;\n" +
                "uniform float uAspect;\n" +
                "uniform vec3 uTint;\n" +
                "void main() {\n" +
                "  vec3 bg = vec3(vUv.x, vUv.y, 0.5 + 0.5 * sin(uTime)) * uTint;\n" +
                "  vec2 d = (vUv - uBall) * vec2(uAspect, 1.0);\n" +
                "  float disc = smoothstep(uBallR, uBallR * 0.85, length(d));\n" +
                "  vec3 col = mix(bg, vec3(0.95), disc);\n" +
                "  gl_FragColor = vec4(col, 1.0);\n" +
                "}\n";

        RenderThread(Surface surface, int width, int height, int fps) {
            super("VCamRenderGL");
            mSurface = surface;
            mWidth = width;
            mHeight = height;
            mFps = fps;
        }

        void quit() {
            mRunning = false;
            interrupt();
        }

        private static int compile(int type, String src) {
            int s = GLES20.glCreateShader(type);
            GLES20.glShaderSource(s, src);
            GLES20.glCompileShader(s);
            int[] ok = new int[1];
            GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
            if (ok[0] == 0) {
                throw new RuntimeException("shader compile: " + GLES20.glGetShaderInfoLog(s));
            }
            return s;
        }

        @Override
        public void run() {
            Log.i(TAG, "GL render thread started " + mWidth + "x" + mHeight + "@" + mFps);
            EGLDisplay dpy = EGL14.EGL_NO_DISPLAY;
            EGLContext ctx = EGL14.EGL_NO_CONTEXT;
            EGLSurface win = EGL14.EGL_NO_SURFACE;
            try {
                dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
                EGL14.eglInitialize(dpy, new int[2], 0, new int[2], 0);
                int[] cfgAttrs = {
                        EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                        EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                        EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8,
                        EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                        EGL14.EGL_NONE };
                EGLConfig[] cfgs = new EGLConfig[1];
                int[] n = new int[1];
                EGL14.eglChooseConfig(dpy, cfgAttrs, 0, cfgs, 0, 1, n, 0);
                if (n[0] == 0) throw new RuntimeException("no EGL config");
                ctx = EGL14.eglCreateContext(dpy, cfgs[0], EGL14.EGL_NO_CONTEXT,
                        new int[] { EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE }, 0);
                win = EGL14.eglCreateWindowSurface(dpy, cfgs[0], mSurface,
                        new int[] { EGL14.EGL_NONE }, 0);
                EGL14.eglMakeCurrent(dpy, win, win, ctx);

                int prog = GLES20.glCreateProgram();
                GLES20.glAttachShader(prog, compile(GLES20.GL_VERTEX_SHADER, VERT));
                GLES20.glAttachShader(prog, compile(GLES20.GL_FRAGMENT_SHADER, FRAG));
                GLES20.glLinkProgram(prog);
                GLES20.glUseProgram(prog);

                java.nio.FloatBuffer quad = java.nio.ByteBuffer
                        .allocateDirect(8 * 4).order(java.nio.ByteOrder.nativeOrder())
                        .asFloatBuffer();
                quad.put(new float[] { -1, -1, 1, -1, -1, 1, 1, 1 }).position(0);
                int aPos = GLES20.glGetAttribLocation(prog, "aPos");
                GLES20.glEnableVertexAttribArray(aPos);
                GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, quad);
                int uTime = GLES20.glGetUniformLocation(prog, "uTime");
                int uBall = GLES20.glGetUniformLocation(prog, "uBall");
                int uBallR = GLES20.glGetUniformLocation(prog, "uBallR");
                int uAspect = GLES20.glGetUniformLocation(prog, "uAspect");
                int uTint = GLES20.glGetUniformLocation(prog, "uTint");
                float aspect = (float) mWidth / Math.max(1, mHeight);
                float[] tint = tintFor(sTint);
                GLES20.glUniform3f(uTint, tint[0], tint[1], tint[2]);
                Log.i(TAG, "Render thread tint preset " + sTint);

                GLES20.glViewport(0, 0, mWidth, mHeight);
                long frameIntervalMs = 1000L / mFps;
                while (mRunning && mSurface.isValid()) {
                    long t0 = System.currentTimeMillis();
                    float t = mFrames / (float) mFps;
                    float phase = (mFrames % 120) / 120f;
                    float bx = 0.1f + 0.8f * phase;
                    float by = 0.5f + (float) (Math.sin(phase * 2 * Math.PI) * 0.3f);
                    GLES20.glUniform1f(uTime, t);
                    GLES20.glUniform2f(uBall, bx, by);
                    GLES20.glUniform1f(uBallR, 0.08f);
                    GLES20.glUniform1f(uAspect, aspect);
                    GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
                    if (!EGL14.eglSwapBuffers(dpy, win)) {
                        Log.w(TAG, "eglSwapBuffers failed (surface gone?)");
                        break;
                    }
                    mFrames++;
                    if (mFrames % 30 == 0) Log.i(TAG, "PRODUCED " + mFrames + " frames (GL)");
                    long sleepMs = frameIntervalMs - (System.currentTimeMillis() - t0);
                    if (sleepMs > 2) {
                        try { Thread.sleep(sleepMs); } catch (InterruptedException e) { /* quitting */ }
                    }
                }
            } catch (Exception e) {
                Log.e(TAG, "GL render failed", e);
            } finally {
                if (dpy != EGL14.EGL_NO_DISPLAY) {
                    EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE,
                            EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                    if (win != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(dpy, win);
                    if (ctx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(dpy, ctx);
                    EGL14.eglTerminate(dpy);
                }
                Log.i(TAG, "GL render thread exiting after " + mFrames + " frames");
            }
        }
    }

    /** 0 = untinted (default), 1 = red-heavy, 2 = green-heavy, 3 = blue-heavy. */
    private static float[] tintFor(int preset) {
        switch (preset) {
            case 1: return new float[] { 1.0f, 0.25f, 0.25f };
            case 2: return new float[] { 0.25f, 1.0f, 0.25f };
            case 3: return new float[] { 0.25f, 0.25f, 1.0f };
            default: return new float[] { 1.0f, 1.0f, 1.0f };
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && intent.hasExtra("tint")) {
            sTint = intent.getIntExtra("tint", 0);
            Log.i(TAG, "tint preset " + sTint + " (" + getPackageName() + ")");
        }
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
