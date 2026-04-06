/*
 * RendererInterface - Zero-copy rendering to virtual camera
 *
 * Used by renderer apps to submit frames with minimal latency.
 * Renderer draws directly to AHardwareBuffer-backed FBOs,
 * then pushes a slot to the control ring with a GPU sync fence.
 */
#pragma once

#include "SharedBufferPool.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/sync.h>
#include <time.h>

namespace virtualcamera {

/**
 * Renderer-side interface for submitting frames.
 *
 * Usage:
 *   RendererInterface renderer;
 *   renderer.initialize(pool, eglGetCurrentDisplay());
 *
 *   // Render loop:
 *   while (running) {
 *       Frame frame = renderer.beginFrame();
 *       if (frame.valid()) {
 *           // frame.framebuffer is already bound
 *           yourRenderFunction();
 *           renderer.submitFrame(frame);
 *       }
 *   }
 */
class RendererInterface {
public:
    struct Frame {
        int bufferIndex = -1;
        AHardwareBuffer* buffer = nullptr;
        EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
        GLuint framebuffer = 0;
        int64_t timestampNs = 0;

        bool valid() const { return bufferIndex >= 0; }
    };

    RendererInterface() = default;
    ~RendererInterface() { shutdown(); }

    /**
     * Initialize with a shared buffer pool.
     * Must be called on a thread with a current EGL context.
     */
    bool initialize(SharedBufferPool* pool, EGLDisplay display) {
        mPool = pool;
        mDisplay = display;

        // Resolve EGL extension function pointers
        mEglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
            eglGetProcAddress("eglCreateImageKHR");
        mEglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
            eglGetProcAddress("eglDestroyImageKHR");
        mEglGetNativeClientBufferANDROID = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
            eglGetProcAddress("eglGetNativeClientBufferANDROID");
        mEglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)
            eglGetProcAddress("eglCreateSyncKHR");
        mEglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)
            eglGetProcAddress("eglDestroySyncKHR");
        mEglDupNativeFenceFDANDROID = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
            eglGetProcAddress("eglDupNativeFenceFDANDROID");
        mGlEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");

        if (!mEglCreateImageKHR || !mEglGetNativeClientBufferANDROID ||
            !mGlEGLImageTargetTexture2DOES) {
            return false;
        }

        // Create EGL images + FBOs for each buffer
        for (int i = 0; i < pool->getBufferCount(); i++) {
            AHardwareBuffer* buffer = pool->getBuffer(i);

            EGLClientBuffer clientBuffer =
                mEglGetNativeClientBufferANDROID(buffer);

            EGLint attrs[] = {
                EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                EGL_NONE
            };

            mEglImages[i] = mEglCreateImageKHR(
                display, EGL_NO_CONTEXT,
                EGL_NATIVE_BUFFER_ANDROID,
                clientBuffer, attrs);

            if (mEglImages[i] == EGL_NO_IMAGE_KHR) {
                shutdown();
                return false;
            }

            // Texture backed by EGL image
            glGenTextures(1, &mTextures[i]);
            glBindTexture(GL_TEXTURE_2D, mTextures[i]);
            mGlEGLImageTargetTexture2DOES(GL_TEXTURE_2D, mEglImages[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            // FBO for rendering to this texture
            glGenFramebuffers(1, &mFramebuffers[i]);
            glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffers[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, mTextures[i], 0);

            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
                GL_FRAMEBUFFER_COMPLETE) {
                shutdown();
                return false;
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        mInitialized = true;
        return true;
    }

    void shutdown() {
        if (!mInitialized) return;

        for (int i = 0; i < MAX_BUFFERS; i++) {
            if (mFramebuffers[i]) {
                glDeleteFramebuffers(1, &mFramebuffers[i]);
                mFramebuffers[i] = 0;
            }
            if (mTextures[i]) {
                glDeleteTextures(1, &mTextures[i]);
                mTextures[i] = 0;
            }
            if (mEglImages[i] != EGL_NO_IMAGE_KHR && mEglDestroyImageKHR) {
                mEglDestroyImageKHR(mDisplay, mEglImages[i]);
                mEglImages[i] = EGL_NO_IMAGE_KHR;
            }
        }

        mInitialized = false;
    }

    /**
     * Begin a new frame.
     * Drains the release ring first so released buffers become available.
     * Returns a Frame with the FBO already bound for rendering.
     * Returns invalid frame if no buffer available (skip this frame).
     */
    Frame beginFrame() {
        Frame frame;

        // Drain release ring — reclaim buffers the HAL is done with
        drainReleaseRing();

        int bufferIdx = mPool->acquireFreeBuffer();
        if (bufferIdx < 0) {
            return frame;  // No free buffer
        }

        frame.bufferIndex = bufferIdx;
        frame.buffer = mPool->getBuffer(bufferIdx);
        frame.eglImage = mEglImages[bufferIdx];
        frame.texture = mTextures[bufferIdx];
        frame.framebuffer = mFramebuffers[bufferIdx];

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        frame.timestampNs = ts.tv_sec * 1000000000LL + ts.tv_nsec;

        glBindFramebuffer(GL_FRAMEBUFFER, frame.framebuffer);

        return frame;
    }

    /**
     * Submit a completed frame.
     * Creates a sync fence and pushes to the control ring.
     */
    bool submitFrame(const Frame& frame) {
        if (!frame.valid()) return false;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Create native fence sync and extract the fd
        int fenceFd = -1;
        if (mEglCreateSyncKHR && mEglDupNativeFenceFDANDROID) {
            EGLSyncKHR sync = mEglCreateSyncKHR(
                mDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);

            glFlush();  // Ensure sync is enqueued

            if (sync != EGL_NO_SYNC_KHR) {
                fenceFd = mEglDupNativeFenceFDANDROID(mDisplay, sync);
                mEglDestroySyncKHR(mDisplay, sync);
            }
        } else {
            // No fence extension — glFinish to ensure GPU is done
            glFinish();
        }

        // Submit to control ring
        ControlRing* ring = mPool->getControlRing();
        bool submitted = ring->tryWrite(
            frame.bufferIndex, fenceFd, frame.timestampNs);

        if (!submitted) {
            // Ring full — release buffer back, close fence
            mPool->releaseBuffer(frame.bufferIndex);
            if (fenceFd >= 0) close(fenceFd);
            return false;
        }

        return true;
    }

    // Accessors
    int getWidth() const { return mPool->getConfig().width; }
    int getHeight() const { return mPool->getConfig().height; }

    /**
     * Check if the HAL has requested a different buffer format.
     * Call this periodically (e.g. on each beginFrame). If it returns
     * true, the renderer should tear down the current pool and
     * reconnect with buffers in the requested format.
     *
     * Returns true if the negotiation sequence has changed since the
     * last call to checkNegotiation() or since initialization.
     */
    struct FormatRequest {
        int32_t format;   // AHARDWAREBUFFER_FORMAT_*
        int32_t width;
        int32_t height;
        uint64_t usage;
    };

    bool checkNegotiation(FormatRequest* out) {
        if (!mPool || !mPool->getControlRing()) return false;

        auto& neg = mPool->getControlRing()->negotiation;
        uint32_t seq = neg.seq.load(std::memory_order_acquire);

        if (seq == mLastNegotiationSeq) return false;

        mLastNegotiationSeq = seq;
        if (out) {
            out->format = neg.format.load(std::memory_order_relaxed);
            out->width = neg.width.load(std::memory_order_relaxed);
            out->height = neg.height.load(std::memory_order_relaxed);
            out->usage = neg.usage.load(std::memory_order_relaxed);
        }
        return true;
    }

private:
    /**
     * Drain all pending release notifications from the HAL.
     * Marks released buffers as free in the pool.
     */
    void drainReleaseRing() {
        if (!mPool || !mPool->getControlRing()) return;

        ControlRing* ring = mPool->getControlRing();
        ControlRing::ReleaseSlot rslot;
        while (ring->tryReadRelease(&rslot)) {
            // bufferState already set to 0 by tryWriteRelease,
            // but releaseBuffer is idempotent
        }
    }

    SharedBufferPool* mPool = nullptr;
    EGLDisplay mDisplay = EGL_NO_DISPLAY;
    bool mInitialized = false;
    uint32_t mLastNegotiationSeq = 0;

    EGLImageKHR mEglImages[MAX_BUFFERS] = {};
    GLuint mTextures[MAX_BUFFERS] = {};
    GLuint mFramebuffers[MAX_BUFFERS] = {};

    // EGL extension function pointers
    PFNEGLCREATEIMAGEKHRPROC mEglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC mEglDestroyImageKHR = nullptr;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC mEglGetNativeClientBufferANDROID = nullptr;
    PFNEGLCREATESYNCKHRPROC mEglCreateSyncKHR = nullptr;
    PFNEGLDESTROYSYNCKHRPROC mEglDestroySyncKHR = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC mEglDupNativeFenceFDANDROID = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC mGlEGLImageTargetTexture2DOES = nullptr;
};

}  // namespace virtualcamera
