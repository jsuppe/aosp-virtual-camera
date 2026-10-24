package com.example.vcamproducer;

/** Latest-only mailbox for ONE graphics owner. No Android dependencies. */
final class RenderRequestQueue<T extends RenderRequestQueue.Resource> {
    interface Resource { void release(); }

    static final class Request<T> {
        final long generation;
        final T value;
        Request(long generation, T value) { this.generation = generation; this.value = value; }
    }

    private long mGeneration;
    private boolean mStopped;
    private Request<T> mPending;

    synchronized void submit(T value) {
        if (mStopped) { value.release(); return; }
        discardPending();
        mPending = new Request<>(++mGeneration, value);
        notifyAll();
    }

    synchronized Request<T> take() throws InterruptedException {
        while (mPending == null && !mStopped) wait();
        Request<T> request = mPending;
        mPending = null; // resource ownership transfers to the graphics owner
        return request;
    }

    synchronized boolean isCurrent(Request<T> request) {
        return !mStopped && request.generation == mGeneration;
    }

    synchronized void cancel() {
        ++mGeneration; // invalidate the active request without releasing its Surface
        discardPending();
        notifyAll();
    }

    synchronized void shutdown() {
        mStopped = true;
        cancel();
    }

    private void discardPending() {
        if (mPending != null) { mPending.value.release(); mPending = null; }
    }
}
