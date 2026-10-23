package com.example.vcamproducer;

import android.os.Handler;
import android.os.Looper;
import android.view.Choreographer;

/** Latest-only display clock. EGL remains exclusively on the graphics owner. */
final class VsyncFrameClock implements AutoCloseable, Choreographer.FrameCallback {
    private final Handler mUi = new Handler(Looper.getMainLooper());
    private Choreographer mChoreographer; // main-thread only
    private boolean mClosed;
    private long mLatest;
    private long mConsumed;

    VsyncFrameClock() {
        mUi.post(() -> {
            synchronized (this) {
                if (mClosed) return;
                mChoreographer = Choreographer.getInstance();
                mChoreographer.postFrameCallback(this);
            }
        });
    }

    @Override public synchronized void doFrame(long frameTimeNanos) {
        if (mClosed) return;
        mLatest = frameTimeNanos;
        notifyAll();
        mChoreographer.postFrameCallback(this);
    }

    synchronized long awaitFrame() throws InterruptedException {
        while (!mClosed && mLatest == mConsumed) wait();
        if (mClosed) throw new InterruptedException("Display clock closed");
        mConsumed = mLatest;
        return mLatest;
    }

    @Override public void close() {
        synchronized (this) {
            mClosed = true;
            notifyAll();
        }
        mUi.post(() -> {
            if (mChoreographer != null) mChoreographer.removeFrameCallback(this);
        });
    }
}
