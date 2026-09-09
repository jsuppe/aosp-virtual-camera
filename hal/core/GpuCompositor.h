/*
 * GpuCompositor - GPU path for filling camera output buffers with zero CPU
 * color conversion.
 *
 * Imports the producer's frame (an AHardwareBuffer that crossed the Treble
 * boundary) as a GL texture and the framework's output gralloc buffer as a
 * GL render target, both via EGLImage (zero-copy imports). A single
 * full-screen shader pass writes the result:
 *
 *   - RGBA/RGBX output  -> passthrough blit (NO color conversion at all)
 *   - anything else (YUV) -> composite() returns false; the caller uses the
 *                            CPU FrameFiller fallback.
 *
 * No pixel ever touches the CPU on the RGBA path. Requires a working
 * EGL/GLES2 stack in the HAL process (present on Cuttlefish gfxstream).
 * Built only when VCAM_GPU_COMPOSITOR is defined.
 */
#pragma once

#ifdef VCAM_GPU_COMPOSITOR

#include <android/hardware_buffer.h>
#include <cutils/native_handle.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <mutex>

#include "HandleImporterCompat.h"

namespace virtualcamera {

class GpuCompositor {
public:
    static GpuCompositor& get();

    /** True if the GPU stack initialized (lazy; safe to poll). */
    bool available();

    /**
     * Blit src (RGBA AHardwareBuffer) into the dst gralloc buffer on the GPU.
     * Returns false if dst is not an RGBA/RGBX format or the GPU path is
     * unavailable, so the caller falls back to the CPU converter.
     *
     * srcAcquireFence: producer's GPU-done fence for src (-1 = none). Not
     *   consumed (the caller keeps ownership); the GPU is told to wait on it
     *   before sampling, so no CPU stall.
     * outFence: receives a native fence (owned by caller, -1 if unavailable)
     *   that signals when the blit — i.e. both the read of src and the write
     *   of dst — has completed on the GPU. When it is provided the call does
     *   NOT wait for the GPU; the caller forwards the fence to whoever reads
     *   dst next and to whoever will overwrite src. When the driver has no
     *   native fence support the call falls back to glFinish() and returns
     *   -1, which callers may treat as "already complete".
     */
    bool composite(AHardwareBuffer* src, int srcAcquireFence, buffer_handle_t dst,
                   int width, int height, int stride, int dstFormat,
                   uint64_t dstUsage, int* outFence);

    /**
     * YUV 4:2:0 output with the color conversion on the GPU.
     *
     * This GL stack (gfxstream on Android 13) has neither GL_EXT_YUV_target
     * nor EGL_EXT_image_dma_buf_import, so a YUV gralloc buffer cannot be a
     * render target. Instead two RGBA passes write *byte-packed* planes —
     * each RGBA texel of the Y target holds 4 consecutive luma bytes, each
     * texel of the UV target holds U0 V0 U1 V1 — into two GPU buffers whose
     * memory is therefore laid out exactly like NV12 planes. Those are then
     * CPU-locked and row-copied into the framework's YUV buffer (dst, already
     * locked by the caller). The per-pixel arithmetic — the expensive part of
     * the old converter — never touches the CPU; what remains is a memcpy of
     * 1.5 bytes per pixel. Handles NV12/NV21 (chroma_step 2) and planar
     * (chroma_step 1) destinations. Synchronous (the CPU copy needs the GPU
     * result). Returns false if the GPU path is unavailable -> CPU fallback.
     */
    bool compositeToYcbcr(AHardwareBuffer* src, int srcAcquireFence,
                          const YCbCrBuffer& dst, int width, int height);

private:
    GpuCompositor() = default;
    bool ensureInit();                       // caller holds mLock
    bool ensureYuvTargets(int width, int height);   // caller holds mLock + context
    void releaseYuvTargets();
    GLuint buildProgram(const char* vert, const char* frag);
    EGLImageKHR imageFromAhb(AHardwareBuffer* ahb);
    EGLImageKHR imageFromHandle(buffer_handle_t h, int w, int hgt, int fmt,
                                uint64_t usage, int stride, void** keepAlive);

    std::mutex mLock;
    bool mTried = false;
    bool mOk = false;

    EGLDisplay mDpy = EGL_NO_DISPLAY;
    EGLContext mCtx = EGL_NO_CONTEXT;
    EGLSurface mPbuf = EGL_NO_SURFACE;

    bool mNativeFence = false;               // EGL_ANDROID_native_fence_sync usable
    GLuint mProg = 0;
    GLuint mSrcTex = 0;
    GLuint mFbo = 0;
    GLint mAttrPos = -1;
    GLint mUniTex = -1;

    // YUV path: packed-plane render targets, cached per output size.
    int mYuvW = 0, mYuvH = 0;
    AHardwareBuffer* mYPack = nullptr;       // RGBA (W/4) x H     == Y plane bytes
    AHardwareBuffer* mUvPack = nullptr;      // RGBA (W/4) x (H/2) == interleaved UV bytes
    EGLImageKHR mYImg = EGL_NO_IMAGE_KHR;
    EGLImageKHR mUvImg = EGL_NO_IMAGE_KHR;
    GLuint mYRb = 0, mUvRb = 0;
    GLuint mProgY = 0, mProgUv = 0;
    GLint mYAttrPos = -1, mYUniTex = -1, mYUniSize = -1;
    GLint mUvAttrPos = -1, mUvUniTex = -1, mUvUniSize = -1, mUvUniSwap = -1;
};

}  // namespace virtualcamera

#endif  // VCAM_GPU_COMPOSITOR
