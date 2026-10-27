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
    static volatile Surface sReferenceSurface; // owned by the split activity
    private static volatile int sProfileRun;
    private static volatile boolean sCachedClock, sRenderOnce;
    private static volatile boolean sMirrorEnabled=true, sClockEnabled=true, sMinimalScene;
    // Immutable handoff: UI requests a change; only the EGL owner paints it.
    private static final class TapState {
        final long sequence, requestedNs;
        final float red, green, blue;
        TapState(long sequence, long requestedNs, float red, float green, float blue) {
            this.sequence = sequence; this.requestedNs = requestedNs;
            this.red = red; this.green = green; this.blue = blue;
        }
    }
    private static volatile TapState sTap;

    static synchronized long changeBackground() {
        long sequence = sTap == null ? 1 : sTap.sequence + 1;
        int color = (int) ((sequence - 1) % 3);
        TapState tap = new TapState(sequence, System.nanoTime(),
                color == 0 ? 0.8f : 0.06f, color == 1 ? 0.65f : 0.06f,
                color == 2 ? 0.85f : 0.06f);
        sTap = tap;
        Log.i(TAG, "TAP_REQUEST sequence=" + sequence + " handlerNs=" + tap.requestedNs);
        return sequence;
    }
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
            IBinder binder = ServiceManager.getService("ruminate_virtual_camera");
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
            config.maxFps = 120;
            config.supportedFormats = new int[] { 1 /* RGBA_8888 */ };
            config.facing = 2;      // EXTERNAL
            config.orientation = 0;
            config.supportsJpeg = !mProfile.endsWith("-nojpeg");
            java.util.ArrayList<ProducerMode> modes = new java.util.ArrayList<>();
            if(mProfile.equals("4k-perf") && sProfileRun>0) {
                modes.add(mode(1,3840,2160,120,config.supportsJpeg));
                modes.add(mode(2,3840,2160,60,config.supportsJpeg));
                modes.add(mode(3,1920,1080,120,config.supportsJpeg));
                modes.add(mode(4,640,480,120,config.supportsJpeg));
                modes.add(mode(5,3840,2160,40,config.supportsJpeg));
                modes.add(mode(6,3840,2160,45,config.supportsJpeg));
                modes.add(mode(7,3840,2160,50,config.supportsJpeg));
            }
            else if(mProfile.startsWith("1080p"))modes.add(mode(1,1920,1080,mProfile.startsWith("1080p60")?60:30,config.supportsJpeg));
            else if(mProfile.startsWith("4k"))modes.add(mode(1,3840,2160,mProfile.startsWith("4k60")?60:30,config.supportsJpeg));
            else if(mProfile.equals("mixed")) {modes.add(mode(1,1920,1080,60,true));modes.add(mode(2,3840,2160,30,true));}
            else if(mProfile.equals("disjoint")) {
                ProducerMode hd=mode(1,1920,1080,60,true);hd.minFps=60;modes.add(hd);
                ProducerMode uhd=mode(2,3840,2160,30,true);uhd.minFps=30;modes.add(uhd);
            }
            else {
                int id = 1;
                for (int[] size : new int[][] {{640,480},{1280,720},{1920,1080},{3840,2160}}) {
                    // VGA/1080p high-rate tests advertise the negotiation ceiling,
                    // not a claim of sustained throughput. Other modes stay at30.
                    modes.add(mode(id++, size[0], size[1],
                            size[0] == 640 || size[0] == 1920 ? 120 : 30,
                            config.supportsJpeg));
                }
                // Capability metadata enumerates endpoint/default FPS ranges,
                // not every integer inside min..max. Preserve explicit60 modes
                // when adding120, so [60,60] remains advertised and admissible.
                modes.add(mode(id++, 640, 480, 60, config.supportsJpeg));
                modes.add(mode(id++, 1920, 1080, 60, config.supportsJpeg));
            }
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
        // Diagnostic solid field with a 16-bit per-frame stripe; no Canvas/texture uploads.
        private static final String MINIMAL_FRAG =
                "precision highp float; varying vec2 vUv; uniform float uProfileFrame;"
                +"void main(){vec3 c=vec3(.08,.18,.3);"
                +"if(vUv.y<.04){float b=floor(vUv.x*16.);"
                +"float bit=mod(floor(uProfileFrame/pow(2.,b)),2.);c=vec3(bit);}"
                +"gl_FragColor=vec4(c,1.);}";
        private static final String FRAG =
                "precision highp float;\n" +
                "varying vec2 vUv;\n" +
                "uniform float uTime;\n" +
                "uniform vec2 uBall;\n" +
                "uniform float uBallR;\n" +
                "uniform float uAspect;\n" +
                "uniform vec3 uTint;\n" +
                "uniform vec4 uTapBackground;\n" +
                "uniform float uClockMs;\n" +
                "uniform sampler2D uClockTexture;\n" +
                "void main() {\n" +
                "  vec3 bg = vec3(vUv.x, vUv.y, 0.5 + 0.5 * sin(uTime)) * uTint;\n" +
                "  bg = mix(bg, uTapBackground.rgb, uTapBackground.a);\n" +
                "  vec2 d = (vUv - uBall) * vec2(uAspect, 1.0);\n" +
                "  float disc = smoothstep(uBallR, uBallR * 0.85, length(d));\n" +
                "  vec3 col = mix(bg, vec3(0.95), disc);\n" +
                "  if(uClockMs>=0.0) {\n" +
                "    vec2 uv=vec2((vUv.x-.07)/.86,(.94-vUv.y)/.84);\n" +
                "    if(uv.x>=0.0 && uv.x<=1.0 && uv.y>=0.0 && uv.y<=1.0) {\n" +
                "      vec4 stamp=texture2D(uClockTexture,uv);\n" +
                "      col=mix(col,stamp.rgb,stamp.a);\n" +
                "    }\n" +
                "  }\n" +
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
                    Thread.interrupted(); // clear the mailbox wake-up before waiting for vsync
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
            int vertexBuffer = 0;
            Log.i(TAG, "GL generation " + request.generation + " started " +
                    mWidth + "x" + mHeight + "@" + mFps);
            ClockTexture clockTexture = new ClockTexture(sCachedClock && sProfileRun>0);
            SceneReuse reuse = null;
            final int profileRun=sProfileRun;
            final boolean minimalScene=sMinimalScene && profileRun>0;
            FrameTrace trace=new FrameTrace(new java.io.File(getFilesDir(),"vcam-profile"),
                    "producer-"+profileRun+"-"+request.generation,
                    "frame,vsync_ns,start_ns,overlay_end_ns,draw_end_ns,swap_end_ns,mirror_end_ns,fps",
                    profileRun>0);
            DirectMirror mirror = new DirectMirror();
            VsyncFrameClock clock = null;
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
                GLES20.glAttachShader(prog, compile(GLES20.GL_FRAGMENT_SHADER, minimalScene ? MINIMAL_FRAG : FRAG));
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
                // GL retains a native pointer for client arrays, not a Java
                // reference. A local direct buffer can be reclaimed mid-loop.
                // Upload once so geometry ownership belongs to the GL context.
                int[] buffers = new int[1];
                GLES20.glGenBuffers(1, buffers, 0);
                vertexBuffer = buffers[0];
                GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vertexBuffer);
                GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER, 8 * 4, quad, GLES20.GL_STATIC_DRAW);
                GLES20.glEnableVertexAttribArray(aPos);
                GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, 0);
                Log.i(TAG, "GEOMETRY GPU-owned vertex buffer=" + vertexBuffer);
                if(sRenderOnce && sProfileRun>0) reuse=new SceneReuse(mWidth,mHeight,vertexBuffer);
                GLES20.glUseProgram(prog);
                int uClockMs = GLES20.glGetUniformLocation(prog, "uClockMs");
                GLES20.glUniform1i(GLES20.glGetUniformLocation(prog, "uClockTexture"), 0);
                int uProfileFrame=GLES20.glGetUniformLocation(prog,"uProfileFrame");
                int uTime = GLES20.glGetUniformLocation(prog, "uTime");
                int uBall = GLES20.glGetUniformLocation(prog, "uBall");
                int uBallR = GLES20.glGetUniformLocation(prog, "uBallR");
                int uAspect = GLES20.glGetUniformLocation(prog, "uAspect");
                int uTint = GLES20.glGetUniformLocation(prog, "uTint");
                int uTapBackground = GLES20.glGetUniformLocation(prog, "uTapBackground");
                float aspect = (float) mWidth / Math.max(1, mHeight);
                float[] tint = tintFor(sTint);
                GLES20.glUniform3f(uTint, tint[0], tint[1], tint[2]);
                Log.i(TAG, "Render thread tint preset " + sTint);

                GLES20.glViewport(0, 0, mWidth, mHeight);
                if(!mRequests.isCurrent(request))return;
                mService.setProducerReady(mCallback,config.connection,config.token,!mRejectReady);
                if(mRejectReady)return;
                clock = new VsyncFrameClock();
                Log.i(TAG, "PACING choreographer-vsync targetFps=" + mFps);
                final long frameIntervalNs = 1000000000L / mFps;
                long nextFrameNs = 0;
                long animationStartNs = 0;
                long lastTapSequence = 0;
                while (mRequests.isCurrent(request) && mSurface.isValid()) {
                    long frameTimeNs = clock.awaitFrame();
                    if (!mRequests.isCurrent(request)) break;
                    MarkerControl marker = sMarker;
                    if (marker != null && marker.paused) {
                        nextFrameNs = 0; // no queued catch-up frames after resume
                        continue;
                    }
                    // Fixed phase, not "work duration + relative sleep". A late
                    // callback skips old deadlines, never renders a catch-up burst.
                    if (nextFrameNs != 0 && frameTimeNs + 500000L < nextFrameNs) continue;
                    if (nextFrameNs == 0) nextFrameNs = frameTimeNs;
                    nextFrameNs += Math.max(1L,
                            (frameTimeNs - nextFrameNs) / frameIntervalNs + 1L) * frameIntervalNs;
                    long profileStart=profileRun>0?System.nanoTime():0;
                    if (animationStartNs == 0) animationStartNs = frameTimeNs;
                    float t = (frameTimeNs - animationStartNs) / 1000000000f;
                    double angle = t * (2 * Math.PI / 4.0);
                    float bx = 0.5f + 0.4f * (float) Math.cos(angle);
                    float by = 0.5f + 0.3f * (float) Math.sin(angle);
                    GLES20.glUseProgram(prog);
                    GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER,vertexBuffer);
                    GLES20.glEnableVertexAttribArray(aPos);
                    GLES20.glVertexAttribPointer(aPos,2,GLES20.GL_FLOAT,false,0,0);
                    if(reuse!=null)reuse.begin();
                    GLES20.glViewport(0, 0, mWidth, mHeight);
                    int cycleFrame = (int) (mFrames % mFps) + 1;
                    int cycleMs = (cycleFrame * 1000 + mFps / 2) / mFps;
                    TapState tap = sTap; // one snapshot for the camera and direct draws
                    if (tap == null) GLES20.glUniform4f(uTapBackground, 0, 0, 0, 0);
                    else GLES20.glUniform4f(uTapBackground, tap.red, tap.green, tap.blue, 1);
                    boolean drawClock=sReferenceSurface!=null && sClockEnabled && !minimalScene;
                    GLES20.glUniform1f(uClockMs, drawClock ? cycleMs : -1f);
                    if (drawClock) clockTexture.update(cycleFrame, mFps, cycleMs);
                    long overlayEnd=profileRun>0?System.nanoTime():0;
                    GLES20.glUniform1f(uProfileFrame,mFrames%65536);
                    GLES20.glUniform1f(uTime, t);
                    GLES20.glUniform2f(uBall, bx, by);
                    GLES20.glUniform1f(uBallR, 0.08f);
                    GLES20.glUniform1f(uAspect, aspect);
                    GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
                    int drawError = GLES20.glGetError();
                    if (drawError != GLES20.GL_NO_ERROR)
                        throw new IllegalStateException("GL draw error=" + drawError);
                    if (marker != null) drawMarker(marker, request.generation, mWidth, mHeight);
                    if(reuse!=null)reuse.toCamera(mWidth,mHeight);
                    long drawEnd=profileRun>0?System.nanoTime():0;
                    if (!mRequests.isCurrent(request)) break;
                    if (!EGL14.eglSwapBuffers(dpy, win)) {
                        Log.w(TAG, "eglSwapBuffers failed (surface gone?)");
                        break;
                    }
                    long swapEnd=profileRun>0?System.nanoTime():0;
                    if (tap != null && tap.sequence != lastTapSequence) {
                        lastTapSequence = tap.sequence;
                        Log.i(TAG, "TAP_ENQUEUED sequence=" + tap.sequence
                                + " handlerToSwapReturnMs="
                                + (System.nanoTime() - tap.requestedNs) / 1000000.0);
                        // Swap return is NOT camera receipt or physical display presentation.
                    }
                    if (marker == null && sMirrorEnabled && mRequests.isCurrent(request)) {
                        mirror.draw(dpy, cfgs[0], ctx, win);
                    }
                    if (profileRun>0) trace.add(mFrames,frameTimeNs,profileStart,overlayEnd,
                            drawEnd,swapEnd,System.nanoTime(),mFps);
                    mFrames++;
                    if (mFrames % 30 == 0) Log.i(TAG, "PRODUCED " + mFrames + " frames (GL)");

                }
            } catch (Exception e) {
                if (mRequests.isCurrent(request)) {
                    try {mService.setProducerReady(mCallback,config.connection,config.token,false);}catch(Exception ignored){}
                    Log.e(TAG, "GL render failed generation " + request.generation, e);
                }
            } finally {
                if (clock != null) clock.close();
                if (dpy != EGL14.EGL_NO_DISPLAY) {
                    if(reuse!=null)reuse.close();
                    clockTexture.close();
                    if (vertexBuffer != 0)
                        GLES20.glDeleteBuffers(1, new int[] { vertexBuffer }, 0);
                    EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE,
                            EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                    mirror.close(dpy);
                    if (win != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(dpy, win);
                    if (ctx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(dpy, ctx);
                    EGL14.eglTerminate(dpy);
                }
                EGL14.eglReleaseThread();
                config.release();
                try { trace.close(); } catch(java.io.IOException e) { Log.e(TAG,"Profile export",e); }
                Log.i(TAG, "GL generation " + request.generation + " finished after " + mFrames + " frames");
            }
        }
    }

    /** Diagnostic text upload once per frame, sampled identically by both GPU outputs. */
    private static final class ClockTexture {
        private android.graphics.Bitmap bitmap;
        private android.graphics.Canvas canvas;
        private final android.graphics.Paint paint = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        private int texture;
        private final boolean cached;
        private ClockTexture[] frames;
        ClockTexture(boolean cached){this.cached=cached;}
        void update(int frame, int fps, int millis) {
            if(cached) {
                if(frames==null) {
                    frames=new ClockTexture[fps];
                    for(int i=0;i<fps;i++) {
                        frames[i]=new ClockTexture(false);
                        frames[i].update(i+1,fps,((i+1)*1000+fps/2)/fps);
                        frames[i].bitmap.recycle(); frames[i].bitmap=null;
                    }
                }
                GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
                GLES20.glBindTexture(GLES20.GL_TEXTURE_2D,frames[frame-1].texture);
                return;
            }
            if (bitmap == null) {
                bitmap = android.graphics.Bitmap.createBitmap(512, 256, android.graphics.Bitmap.Config.ARGB_8888);
                canvas = new android.graphics.Canvas(bitmap);
                paint.setTypeface(android.graphics.Typeface.MONOSPACE);
                int[] ids = new int[1]; GLES20.glGenTextures(1, ids, 0); texture = ids[0];
                GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
                GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture);
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
                GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
                android.opengl.GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, bitmap, 0);
            }
            canvas.drawColor(android.graphics.Color.TRANSPARENT, android.graphics.PorterDuff.Mode.CLEAR);
            paint.setColor(0xff050505); canvas.drawRect(0, 0, 512, 126, paint);
            paint.setColor(0xfffff280); paint.setTextAlign(android.graphics.Paint.Align.CENTER); paint.setTextSize(88);
            canvas.drawText(String.format(java.util.Locale.ROOT, "%d.%03d", millis / 1000, millis % 1000), 256, 84, paint);
            paint.setTextSize(28);
            canvas.drawText(String.format(java.util.Locale.ROOT, "FRAME %02d / %02d", frame, fps), 256, 116, paint);
            for (int i = 0; i < fps; ++i) {
                paint.setColor(i < frame ? 0xff19e6cc : 0xff060606);
                canvas.drawRect(i * 512f / fps + 1, 240, (i + 1) * 512f / fps - 1, 256, paint);
            }
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture);
            android.opengl.GLUtils.texSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, bitmap);
        }
        void close() {
            if(frames!=null){for(ClockTexture frame:frames)if(frame!=null)frame.close();frames=null;}
            if (texture != 0) { GLES20.glDeleteTextures(1, new int[] { texture }, 0); texture = 0; }
            if (bitmap != null) { bitmap.recycle(); bitmap = null; }
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
        if (intent!=null) {
            sProfileRun=intent.getIntExtra("profile_run",0);
            sCachedClock=intent.getBooleanExtra("cached_clock",false);
            sRenderOnce=intent.getBooleanExtra("render_once",false);
            sMirrorEnabled=intent.getBooleanExtra("mirror",true);
            sClockEnabled=intent.getBooleanExtra("clock",true);
            sMinimalScene=intent.getBooleanExtra("minimal_scene",false);
        }
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
