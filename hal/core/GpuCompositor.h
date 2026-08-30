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
     */
    bool composite(AHardwareBuffer* src, buffer_handle_t dst,
                   int width, int height, int stride, int dstFormat,
                   uint64_t dstUsage);

private:
    GpuCompositor() = default;
    bool ensureInit();                       // caller holds mLock
    EGLImageKHR imageFromAhb(AHardwareBuffer* ahb);
    EGLImageKHR imageFromHandle(buffer_handle_t h, int w, int hgt, int fmt,
                                uint64_t usage, int stride, void** keepAlive);

    std::mutex mLock;
    bool mTried = false;
    bool mOk = false;

    EGLDisplay mDpy = EGL_NO_DISPLAY;
    EGLContext mCtx = EGL_NO_CONTEXT;
    EGLSurface mPbuf = EGL_NO_SURFACE;

    GLuint mProg = 0;
    GLuint mSrcTex = 0;
    GLuint mFbo = 0;
    GLint mAttrPos = -1;
    GLint mUniTex = -1;
};

}  // namespace virtualcamera

#endif  // VCAM_GPU_COMPOSITOR
