package com.example.vcamproducer;

import android.opengl.*;
import android.util.Log;
import android.view.Surface;

/** Optional diagnostic output, used only by the single EGL owner. */
final class DirectMirror {
    private Surface mSource;
    private EGLSurface mWindow = EGL14.EGL_NO_SURFACE;
    private boolean mLogged;

    void draw(EGLDisplay display, EGLConfig config, EGLContext context,
            EGLSurface cameraWindow) {
        Surface source = VCamProducerService.sReferenceSurface;
        if (source != mSource) {
            close(display);
            mSource = source;
            if (source != null && source.isValid()) {
                try {
                    mWindow = EGL14.eglCreateWindowSurface(display, config, source,
                            new int[] { EGL14.EGL_NONE }, 0);
                } catch (IllegalArgumentException gone) {
                    Log.w("VCamSplit", "Reference surface closed before EGL attachment");
                }
            }
        }
        if (mWindow == EGL14.EGL_NO_SURFACE) return;
        try {
            if (!EGL14.eglMakeCurrent(display, mWindow, mWindow, context)) {
                close(display);
                return;
            }
            int[] width = new int[1], height = new int[1];
            EGL14.eglQuerySurface(display, mWindow, EGL14.EGL_WIDTH, width, 0);
            EGL14.eglQuerySurface(display, mWindow, EGL14.EGL_HEIGHT, height, 0);
            GLES20.glViewport(0, 0, width[0], height[0]);
            // Same context, program, clock uniforms and animation as the frame
            // just queued to the HAL. No independent reference clock/render loop.
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
            if (!EGL14.eglSwapBuffers(display, mWindow)) {
                Log.w("VCamSplit", "Reference surface no longer accepting frames");
                close(display);
            } else if (!mLogged) {
                Log.i("VCamSplit", "Direct producer mirror active " + width[0] + "x" + height[0]);
                mLogged = true;
            }
        } finally {
            if (!EGL14.eglMakeCurrent(display, cameraWindow, cameraWindow, context)) {
                throw new IllegalStateException("Cannot restore camera EGL target");
            }
        }
    }

    void close(EGLDisplay display) {
        if (mWindow != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, mWindow);
        mWindow = EGL14.EGL_NO_SURFACE;
        mLogged = false;
    }
}
