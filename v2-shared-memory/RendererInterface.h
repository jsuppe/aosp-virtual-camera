/*
 * RendererInterface - Zero-copy rendering to virtual camera
 * 
 * Used by renderer apps to submit frames with minimal latency.
 */
#pragma once

#include "SharedBufferPool.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/sync.h>
#include <time.h>

namespace virtualcamera {

/**
 * Renderer-side interface for submitting frames.
 * 
 * Usage:
 *   RendererInterface renderer;
 *   renderer.initialize(pool);
 *   
 *   // Render loop:
 *   while (running) {
 *       Frame frame = renderer.beginFrame();
 *       if (frame.valid()) {
 *           // Render to frame.eglImage or frame.buffer
 *           yourRenderFunction(frame);
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
     * Call after pool is set up.
     */
    bool initialize(SharedBufferPool* pool, EGLDisplay display) {
        mPool = pool;
        mDisplay = display;
        
        // Create EGL images for each buffer
        for (int i = 0; i < pool->getBufferCount(); i++) {
            AHardwareBuffer* buffer = pool->getBuffer(i);
            
            EGLClientBuffer clientBuffer = 
                eglGetNativeClientBufferANDROID(buffer);
            
            EGLint attrs[] = {
                EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                EGL_NONE
            };
            
            mEglImages[i] = eglCreateImageKHR(
                display, EGL_NO_CONTEXT, 
                EGL_NATIVE_BUFFER_ANDROID,
                clientBuffer, attrs);
            
            if (mEglImages[i] == EGL_NO_IMAGE_KHR) {
                return false;
            }
            
            // Create texture backed by EGL image
            glGenTextures(1, &mTextures[i]);
            glBindTexture(GL_TEXTURE_2D, mTextures[i]);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, mEglImages[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            
            // Create framebuffer for rendering to this texture
            glGenFramebuffers(1, &mFramebuffers[i]);
            glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffers[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, mTextures[i], 0);
            
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != 
                GL_FRAMEBUFFER_COMPLETE) {
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
            if (mEglImages[i] != EGL_NO_IMAGE_KHR) {
                eglDestroyImageKHR(mDisplay, mEglImages[i]);
                mEglImages[i] = EGL_NO_IMAGE_KHR;
            }
        }
        
        mInitialized = false;
    }
    
    /**
     * Begin a new frame.
     * Returns a Frame object with buffer/texture/framebuffer to render to.
     * Returns invalid frame if no buffer available (skip frame).
     */
    Frame beginFrame() {
        Frame frame;
        
        int bufferIdx = mPool->acquireFreeBuffer();
        if (bufferIdx < 0) {
            // No free buffer - skip this frame
            return frame;
        }
        
        frame.bufferIndex = bufferIdx;
        frame.buffer = mPool->getBuffer(bufferIdx);
        frame.eglImage = mEglImages[bufferIdx];
        frame.texture = mTextures[bufferIdx];
        frame.framebuffer = mFramebuffers[bufferIdx];
        
        // Get current time
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        frame.timestampNs = ts.tv_sec * 1000000000LL + ts.tv_nsec;
        
        // Bind framebuffer for rendering
        glBindFramebuffer(GL_FRAMEBUFFER, frame.framebuffer);
        
        return frame;
    }
    
    /**
     * Submit a completed frame.
     * Creates a sync fence and pushes to the control ring.
     */
    bool submitFrame(const Frame& frame) {
        if (!frame.valid()) return false;
        
        // Unbind framebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        
        // Create sync object
        EGLSyncKHR sync = eglCreateSyncKHR(
            mDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
        
        // Flush to ensure sync is created
        glFlush();
        
        // Get fence fd
        int fenceFd = -1;
        if (sync != EGL_NO_SYNC_KHR) {
            fenceFd = eglDupNativeFenceFDANDROID(mDisplay, sync);
            eglDestroySyncKHR(mDisplay, sync);
        }
        
        // Submit to control ring
        ControlRing* ring = mPool->getControlRing();
        bool submitted = ring->tryWrite(
            frame.bufferIndex, fenceFd, frame.timestampNs);
        
        if (!submitted) {
            // Ring full, release buffer back
            mPool->releaseBuffer(frame.bufferIndex);
            if (fenceFd >= 0) close(fenceFd);
            return false;
        }
        
        // Buffer will be released when HAL is done with it
        return true;
    }
    
    /**
     * Called when HAL releases a buffer back.
     */
    void onBufferReleased(int bufferIndex) {
        mPool->releaseBuffer(bufferIndex);
    }
    
    // Accessors
    int getWidth() const { return mPool->getConfig().width; }
    int getHeight() const { return mPool->getConfig().height; }
    
private:
    SharedBufferPool* mPool = nullptr;
    EGLDisplay mDisplay = EGL_NO_DISPLAY;
    bool mInitialized = false;
    
    EGLImageKHR mEglImages[MAX_BUFFERS] = {};
    GLuint mTextures[MAX_BUFFERS] = {};
    GLuint mFramebuffers[MAX_BUFFERS] = {};
};

}  // namespace virtualcamera
