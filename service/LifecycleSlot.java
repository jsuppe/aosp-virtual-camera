package com.android.server.camera.virtual;

/** A slot remains reserved until its owner's external teardown completes. */
final class LifecycleSlot<T> {
    private T mOwner;
    private boolean mClosing;
    private long mConnection, mEvent;
    private long mConfiguration;
    private boolean mConfigurationClosed;

    synchronized boolean reserve(T owner) {
        if (mOwner != null) return false;
        mOwner = owner;
        return true;
    }
    synchronized T current() { return mClosing ? null : mOwner; }
    synchronized boolean owns(T owner) { return mOwner == owner && !mClosing; }
    synchronized boolean beginClose(T owner) {
        if (!owns(owner)) return false;
        mClosing = true;
        mConfigurationClosed = true;
        ++mEvent;
        return true;
    }
    synchronized long nextEvent(T owner, long connection) {
        if (!owns(owner) || connection == 0 || Long.compareUnsigned(connection, mConnection) < 0) return 0;
        mConnection = connection;
        return ++mEvent;
    }
    synchronized boolean isCurrentEvent(T owner, long event) {
        return owns(owner) && event != 0 && mEvent == event;
    }
    synchronized long nextConfigurationEvent(T owner, long connection, long token, boolean closed) {
        if (!owns(owner) || connection == 0 || token <= 0 ||
                Long.compareUnsigned(connection, mConnection) < 0) return 0;
        if (connection != mConnection) { mConfiguration = 0; mConfigurationClosed = false; }
        if (token < mConfiguration || (token == mConfiguration && (!closed || mConfigurationClosed))) return 0;
        mConnection = connection;
        mConfiguration = token;
        mConfigurationClosed = closed;
        return ++mEvent;
    }
    synchronized boolean finishClose(T owner) {
        if (mOwner != owner || !mClosing) return false;
        mOwner = null;
        mClosing = false;
        return true;
    }
}
