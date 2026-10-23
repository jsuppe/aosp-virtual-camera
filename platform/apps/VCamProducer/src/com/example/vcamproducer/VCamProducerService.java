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
import android.hardware.virtualcamera.ProducerMode;
import android.hardware.virtualcamera.VirtualCameraConfig;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.util.Log;
import android.view.Surface;
import com.example.vcamtest.FrameIdentity;

public class VCamProducerService extends Service {
    private static final String TAG = "VCamProducer";
    private static final String CHANNEL_ID = "vcam_producer";

    private volatile IVirtualCameraService mService;
    private volatile int mCameraId = -1;
    private volatile boolean mDestroyed;
    private boolean mRegistrationStarted;
    private String mProfile = "default";
    private boolean mRejectReady;

    private HandlerThread mRegisterThread;
    private Handler mRegisterHandler;

    // One graphics owner for the service lifetime: replacement never overlaps
    // the old generation's EGL cleanup and never blocks a Binder callback.
    private final RenderThread mRenderThread = new RenderThread();
    /** --ei tint N: colour multiplier preset so two producers are told apart on screen. */
    private static volatile int sTint = 0;
    // Opt-in diagnostic controls; no HAL/AIDL or normal demo behavior changes.
    private static final class MarkerControl {
        final int producer, run;
        final boolean paused;
        MarkerControl(int producer, int run, boolean paused) {
            this.producer = producer; this.run = run; this.paused = paused;
        }
    }
    private static volatile MarkerControl sMarker;

    @Override
    public void onCreate() {
        super.onCreate();
        sMarker = null;
        startForegroundWithNotification();
        mRenderThread.start();

        mRegisterThread = new HandlerThread("VCamRegister");
        mRegisterThread.start();
        mRegisterHandler = new Handler(mRegisterThread.getLooper());
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
        if (mDestroyed || mCameraId >= 0) return;
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
            config.maxWidth = 3840;
            config.maxHeight = 1080;
            config.maxFps = 30;
            config.supportedFormats = new int[] { 1 /* RGBA_8888 */ };
            config.facing = 2;      // EXTERNAL
            config.orientation = 0;
            config.supportsJpeg = !mProfile.endsWith("-nojpeg");
            java.util.ArrayList<ProducerMode> modes = new java.util.ArrayList<>();
            if(mProfile.startsWith("1080p"))modes.add(mode(1,1920,1080,mProfile.startsWith("1080p60")?60:30,config.supportsJpeg));
            else if(mProfile.startsWith("4k"))modes.add(mode(1,3840,2160,mProfile.startsWith("4k60")?60:30,config.supportsJpeg));
            else if(mProfile.equals("mixed")) {modes.add(mode(1,1920,1080,60,true));modes.add(mode(2,3840,2160,30,true));}
            else if(mProfile.equals("disjoint")) {
                ProducerMode hd=mode(1,1920,1080,60,true);hd.minFps=60;modes.add(hd);
                ProducerMode uhd=mode(2,3840,2160,30,true);uhd.minFps=30;modes.add(uhd);
            }
            else {int id=1;for(int[] size:new int[][]{{640,480},{1280,720},{1920,1080},{3840,2160}})modes.add(mode(id++,size[0],size[1],30,config.supportsJpeg));}
            config.modes = modes.toArray(new ProducerMode[0]);
            if(mProfile.equals("legacy-1080p30")){config.modes=null;config.maxWidth=1920;config.maxHeight=1080;config.maxFps=30;}
            if(mProfile.equals("invalid"))config.modes[0].width=8194;
            config.uniqueId = "vcam-producer-1";

            mCameraId = mService.registerCamera(config, mCallback);
            if(mCameraId<0)throw new IllegalStateException("registration not accepted");
            if (mDestroyed) {
                mService.unregisterCamera(mCameraId);
                return;
            }
            Log.i(TAG, "REGISTERED as producer, virtual camera id=" + mCameraId);
        } catch (Exception e) {
            Log.e(TAG, "register failed, retrying in 2s", e);
            mRegisterHandler.postDelayed(this::registerWithRetry, 2000);
        }
    }

    private static ProducerMode mode(int id,int width,int height,int maxFps,boolean jpeg) {
        ProducerMode m=new ProducerMode();m.id=id;m.width=width;m.height=height;m.minFps=15;m.maxFps=maxFps;
        m.outputFormats=jpeg?new int[]{1,34,35,33}:new int[]{1,34,35};m.allowDownscale=true;return m;
    }

    private final IVirtualCameraCallback.Stub mCallback = new IVirtualCameraCallback.Stub() {
        @Override
        public void onCameraOpened() {
            Log.i(TAG, "Callback: onCameraOpened");
        }

        @Override
        public void onStreamsConfigured(StreamConfig[] streams, Surface[] surfaces) {
            Log.i(TAG, "Callback: onStreamsConfigured, " + (streams == null ? 0 : streams.length) + " stream(s)");
            if (streams == null || surfaces == null || streams.length == 0 ||
                    surfaces.length == 0 || streams[0] == null || surfaces[0] == null) {
                releaseSurfaces(surfaces);
                return;
            }
            StreamConfig s = streams[0];
            Log.i(TAG, "Stream 0: " + s.width + "x" + s.height + " fmt=" + s.format
                    + " fps=" + s.fps);
            if (s.width <= 0 || s.height <= 0) { releaseSurfaces(surfaces); return; }
            mRenderThread.replace(new RenderConfig(surfaces[0], s.width, s.height,
                    s.fps > 0 ? s.fps : 30, s.configurationToken, s.connectionToken));
            // This producer consumes only the primary input Surface.
            for (int i = 1; i < surfaces.length; ++i) if (surfaces[i] != null) surfaces[i].release();
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
            mRenderThread.cancel();
        }

        @Override
        public void onStillCaptureRequested(Surface surface, int captureId) {
            Log.i(TAG, "Callback: onStillCaptureRequested " + captureId);
        }
    };

    private static void releaseSurfaces(Surface[] surfaces) {
        if (surfaces != null) for (Surface surface : surfaces) if (surface != null) surface.release();
    }

    private static final class RenderConfig implements RenderRequestQueue.Resource {
        final Surface surface;
        final int width, height, fps;
        final long token, connection;
        RenderConfig(Surface surface, int width, int height, int fps, long token, long connection) {
            this.token = token; this.connection = connection;
            this.surface = surface; this.width = width; this.height = height; this.fps = fps;
        }
        @Override public void release() { surface.release(); }
    }

    /**
     * Renders an animated scene into the Surface with OpenGL ES 2.0 (GPU).
     * The Surface is an ANativeWindow whose buffers EGL allocates with GPU
     * render-target usage, so no CPU touches the pixels on the producer side.
     * A single full-screen fragment shader draws a time-varying gradient plus
     * a moving disc — clearly GPU-generated, and RGBA so the HAL can consume
     * it with zero color conversion.
     */
    private class RenderThread extends Thread {
        private final RenderRequestQueue<RenderConfig> mRequests = new RenderRequestQueue<>();
        private long mMarkerSequence;

        private void drawMarker(MarkerControl control, long generation, int width, int height) {
            byte[] bits = FrameIdentity.encode(control.producer, control.run,
                    Math.toIntExact(generation), ++mMarkerSequence);
            GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
            for (int row = 0; row < FrameIdentity.ROWS; row++) {
                int y0 = row * height / FrameIdentity.ROWS;
                int y1 = (row + 1) * height / FrameIdentity.ROWS;
                for (int col = 0; col < FrameIdentity.COLS; col++) {
                    int x0 = col * width / FrameIdentity.COLS;
                    int x1 = (col + 1) * width / FrameIdentity.COLS;
                    float value = FrameIdentity.bit(bits, row * FrameIdentity.COLS + col) ? 1 : 0;
                    GLES20.glScissor(x0, height - y1, x1 - x0, y1 - y0);
                    GLES20.glClearColor(value, value, value, 1);
                    GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                }
            }
            GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
        }

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

        RenderThread() {
            super("VCamRenderGL");
        }

        void quit() {
            mRequests.shutdown();
            interrupt();
        }

        void replace(RenderConfig config) {
            mRequests.submit(config);
            interrupt();
        }

        void cancel() {
            mRequests.cancel();
            interrupt();
        }

        private int compile(int type, String src) {
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
            for (;;) {
                try {
                    RenderRequestQueue.Request<RenderConfig> request = mRequests.take();
                    if (request == null) return;
                    render(request);
                } catch (InterruptedException e) {
                    // New configuration/cancel/shutdown: recheck the mailbox.
                }
            }
        }

        private void render(RenderRequestQueue.Request<RenderConfig> request) {
            final RenderConfig config = request.value;
            final Surface mSurface = config.surface;
            final int mWidth = config.width, mHeight = config.height, mFps = config.fps;
            long mFrames = 0;
            Log.i(TAG, "GL generation " + request.generation + " started " +
                    mWidth + "x" + mHeight + "@" + mFps);
            EGLDisplay dpy = EGL14.EGL_NO_DISPLAY;
            EGLContext ctx = EGL14.EGL_NO_CONTEXT;
            EGLSurface win = EGL14.EGL_NO_SURFACE;
            try {
                if (!mRequests.isCurrent(request) || !mSurface.isValid()) return;
                dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
                if (dpy == EGL14.EGL_NO_DISPLAY ||
                        !EGL14.eglInitialize(dpy, new int[2], 0, new int[2], 0)) {
                    throw new RuntimeException("eglInitialize: " + EGL14.eglGetError());
                }
                if (!mRequests.isCurrent(request)) return;
                int[] cfgAttrs = {
                        EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                        EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                        EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8,
                        EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                        EGL14.EGL_NONE };
                EGLConfig[] cfgs = new EGLConfig[1];
                int[] n = new int[1];
                if (!EGL14.eglChooseConfig(dpy, cfgAttrs, 0, cfgs, 0, 1, n, 0) || n[0] == 0) {
                    throw new RuntimeException("no EGL config: " + EGL14.eglGetError());
                }
                ctx = EGL14.eglCreateContext(dpy, cfgs[0], EGL14.EGL_NO_CONTEXT,
                        new int[] { EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE }, 0);
                if (ctx == EGL14.EGL_NO_CONTEXT) throw new RuntimeException("eglCreateContext: " + EGL14.eglGetError());
                if (!mRequests.isCurrent(request)) return;
                win = EGL14.eglCreateWindowSurface(dpy, cfgs[0], mSurface,
                        new int[] { EGL14.EGL_NONE }, 0);
                if (win == EGL14.EGL_NO_SURFACE || !EGL14.eglMakeCurrent(dpy, win, win, ctx)) {
                    throw new RuntimeException("eglMakeCurrent/window: " + EGL14.eglGetError());
                }
                if (!mRequests.isCurrent(request)) return;

                int prog = GLES20.glCreateProgram();
                GLES20.glAttachShader(prog, compile(GLES20.GL_VERTEX_SHADER, VERT));
                GLES20.glAttachShader(prog, compile(GLES20.GL_FRAGMENT_SHADER, FRAG));
                GLES20.glLinkProgram(prog);
                int[] linked = new int[1];
                GLES20.glGetProgramiv(prog, GLES20.GL_LINK_STATUS, linked, 0);
                if (linked[0] == 0) throw new RuntimeException("program link: " + GLES20.glGetProgramInfoLog(prog));
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
                if(!mRequests.isCurrent(request))return;
                mService.setProducerReady(mCallback,config.connection,config.token,!mRejectReady);
                if(mRejectReady)return;
                long frameIntervalNs = 1000000000L / mFps;
                while (mRequests.isCurrent(request) && mSurface.isValid()) {
                    MarkerControl marker = sMarker;
                    if (marker != null && marker.paused) {
                        try { Thread.sleep(10); } catch (InterruptedException e) { /* recheck */ }
                        continue;
                    }
                    long t0 = System.nanoTime();
                    float t = mFrames / (float) mFps;
                    float phase = (mFrames % 120) / 120f;
                    float bx = 0.1f + 0.8f * phase;
                    float by = 0.5f + (float) (Math.sin(phase * 2 * Math.PI) * 0.3f);
                    GLES20.glUniform1f(uTime, t);
                    GLES20.glUniform2f(uBall, bx, by);
                    GLES20.glUniform1f(uBallR, 0.08f);
                    GLES20.glUniform1f(uAspect, aspect);
                    GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
                    if (marker != null) drawMarker(marker, request.generation, mWidth, mHeight);
                    if (!mRequests.isCurrent(request)) break;
                    if (!EGL14.eglSwapBuffers(dpy, win)) {
                        Log.w(TAG, "eglSwapBuffers failed (surface gone?)");
                        break;
                    }
                    mFrames++;
                    if (mFrames % 30 == 0) Log.i(TAG, "PRODUCED " + mFrames + " frames (GL)");
                    long remaining = frameIntervalNs - (System.nanoTime() - t0);
                    if (remaining > 0) {
                        try { Thread.sleep(remaining / 1000000L, (int)(remaining % 1000000L)); } catch (InterruptedException e) { /* quitting */ }
                    }
                }
            } catch (Exception e) {
                if (mRequests.isCurrent(request)) {
                    try {mService.setProducerReady(mCallback,config.connection,config.token,false);}catch(Exception ignored){}
                    Log.e(TAG, "GL render failed generation " + request.generation, e);
                }
            } finally {
                if (dpy != EGL14.EGL_NO_DISPLAY) {
                    EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE,
                            EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                    if (win != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(dpy, win);
                    if (ctx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(dpy, ctx);
                    EGL14.eglTerminate(dpy);
                }
                EGL14.eglReleaseThread();
                config.release();
                Log.i(TAG, "GL generation " + request.generation + " finished after " + mFrames + " frames");
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
        if (intent != null && intent.hasExtra("e2e")) {
            if (intent.getBooleanExtra("e2e", false)) {
                int producer = intent.getIntExtra("producer", getPackageName().endsWith("2") ? 2 : 1);
                int run = intent.getIntExtra("run", 0);
                if (producer < 1 || producer > 255 || run <= 0) {
                    Log.e(TAG, "Invalid E2E identity; stopping diagnostic service");
                    stopSelf();
                    return START_NOT_STICKY;
                }
                sMarker = new MarkerControl(producer, run, intent.getBooleanExtra("paused", false));
                Log.i(TAG, "E2E control producer=" + producer + " run=" + run + " paused=" + sMarker.paused);
            } else {
                sMarker = null;
            }
        }
        if (intent != null && intent.hasExtra("tint")) {
            sTint = intent.getIntExtra("tint", 0);
            Log.i(TAG, "tint preset " + sTint + " (" + getPackageName() + ")");
        }
        if (!mRegistrationStarted) {
            if(intent!=null){mProfile=intent.getStringExtra("profile");if(mProfile==null)mProfile="default";mRejectReady=intent.getBooleanExtra("reject_ready",false);}
            mRegistrationStarted = true;
            mRegisterHandler.post(this::registerWithRetry);
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        mDestroyed = true;
        mRenderThread.quit();
        try {
            mRenderThread.join(2000);
            if (mRenderThread.isAlive()) Log.w(TAG, "Graphics cleanup still pending; no replacement will start");
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
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
