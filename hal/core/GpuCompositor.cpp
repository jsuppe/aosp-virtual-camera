/*
 * GpuCompositor implementation.
 */

#ifdef VCAM_GPU_COMPOSITOR

#define LOG_TAG "VCamGpuCompositor"

#include "GpuCompositor.h"

#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <vndk/hardware_buffer.h>
#include <android/sync.h>
#include <log/log.h>
#include <unistd.h>

#include <cstring>

namespace virtualcamera {

namespace {

// HAL_PIXEL_FORMAT values we treat as "renderable RGBA" (no conversion).
constexpr int kFmtRGBA8888 = 1;   // HAL_PIXEL_FORMAT_RGBA_8888
constexpr int kFmtRGBX8888 = 2;   // HAL_PIXEL_FORMAT_RGBX_8888

// Extension entry points (loaded once via eglGetProcAddress).
PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES_ = nullptr;
PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBufferANDROID_ = nullptr;
PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR_ = nullptr;
PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR_ = nullptr;
PFNEGLWAITSYNCKHRPROC eglWaitSyncKHR_ = nullptr;
PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID_ = nullptr;

bool hasExtension(const char* list, const char* name) {
    if (!list) return false;
    const size_t n = strlen(name);
    for (const char* p = list; (p = strstr(p, name)) != nullptr; p += n) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    }
    return false;
}

const char* kVert =
        "attribute vec2 aPos;\n"
        "varying vec2 vUv;\n"
        "void main() {\n"
        "  vUv = aPos * 0.5 + 0.5;\n"
        "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";
const char* kFrag =
        "precision mediump float;\n"
        "varying vec2 vUv;\n"
        "uniform sampler2D uTex;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(uTex, vUv);\n"
        "}\n";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        ALOGE("shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

GpuCompositor& GpuCompositor::get() {
    static GpuCompositor inst;
    return inst;
}

bool GpuCompositor::available() {
    std::lock_guard<std::mutex> lock(mLock);
    return ensureInit();
}

bool GpuCompositor::ensureInit() {
    if (mTried) return mOk;
    mTried = true;

    eglCreateImageKHR_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
            eglGetProcAddress("eglCreateImageKHR"));
    eglDestroyImageKHR_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
            eglGetProcAddress("eglDestroyImageKHR"));
    glEGLImageTargetTexture2DOES_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    glEGLImageTargetRenderbufferStorageOES_ =
            reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
                    eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    eglGetNativeClientBufferANDROID_ = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
            eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    if (!eglCreateImageKHR_ || !eglDestroyImageKHR_ ||
        !glEGLImageTargetTexture2DOES_ || !glEGLImageTargetRenderbufferStorageOES_ ||
        !eglGetNativeClientBufferANDROID_) {
        ALOGE("required EGL/GLES extensions unavailable; GPU path disabled");
        return false;
    }

    mDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (mDpy == EGL_NO_DISPLAY || !eglInitialize(mDpy, nullptr, nullptr)) {
        ALOGE("eglInitialize failed");
        return false;
    }
    const EGLint cfgAttrs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(mDpy, cfgAttrs, &cfg, 1, &n) || n == 0) {
        ALOGE("eglChooseConfig failed");
        return false;
    }
    const EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    mCtx = eglCreateContext(mDpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (mCtx == EGL_NO_CONTEXT) {
        ALOGE("eglCreateContext failed");
        return false;
    }
    const EGLint pbAttrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    mPbuf = eglCreatePbufferSurface(mDpy, cfg, pbAttrs);
    if (!eglMakeCurrent(mDpy, mPbuf, mPbuf, mCtx)) {
        ALOGE("eglMakeCurrent failed");
        return false;
    }

    mProg = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) return false;
    glAttachShader(mProg, vs);
    glAttachShader(mProg, fs);
    glLinkProgram(mProg);
    GLint linked = 0;
    glGetProgramiv(mProg, GL_LINK_STATUS, &linked);
    if (!linked) {
        ALOGE("program link failed");
        return false;
    }
    mAttrPos = glGetAttribLocation(mProg, "aPos");
    mUniTex = glGetUniformLocation(mProg, "uTex");
    glGenTextures(1, &mSrcTex);
    glGenFramebuffers(1, &mFbo);

    // Native fence sync lets the GPU wait on the producer's fence and hand
    // us a fence for our own work, so nothing on the CPU ever blocks on the
    // GPU. Without it we fall back to glFinish() (correct, just slower).
    eglCreateSyncKHR_ = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(
            eglGetProcAddress("eglCreateSyncKHR"));
    eglDestroySyncKHR_ = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
            eglGetProcAddress("eglDestroySyncKHR"));
    eglWaitSyncKHR_ = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(
            eglGetProcAddress("eglWaitSyncKHR"));
    eglDupNativeFenceFDANDROID_ = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
            eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    mNativeFence = hasExtension(eglQueryString(mDpy, EGL_EXTENSIONS),
                                "EGL_ANDROID_native_fence_sync") &&
                   eglCreateSyncKHR_ && eglDestroySyncKHR_ && eglWaitSyncKHR_ &&
                   eglDupNativeFenceFDANDROID_;

    mOk = true;
    ALOGI("GPU compositor initialized (%s), native fences: %s",
          glGetString(GL_RENDERER), mNativeFence ? "yes" : "no (glFinish fallback)");
    // Release the context from this thread; composite() re-acquires it on the
    // (binder-pool) thread that actually processes each capture request. An EGL
    // context can only be current on one thread at a time.
    eglMakeCurrent(mDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return true;
}

EGLImageKHR GpuCompositor::imageFromAhb(AHardwareBuffer* ahb) {
    EGLClientBuffer client = eglGetNativeClientBufferANDROID_(ahb);
    if (!client) return EGL_NO_IMAGE_KHR;
    const EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    return eglCreateImageKHR_(mDpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                              client, attrs);
}

EGLImageKHR GpuCompositor::imageFromHandle(buffer_handle_t h, int w, int hgt,
                                           int fmt, uint64_t usage, int stride,
                                           void** keepAlive) {
    // Wrap the framework's output handle as an AHardwareBuffer (CLONE dups the
    // fds), then take it as a native buffer for EGL. Symmetric with the source
    // path and needs no separate stride bookkeeping.
    AHardwareBuffer_Desc desc = {};
    desc.width = static_cast<uint32_t>(w);
    desc.height = static_cast<uint32_t>(hgt);
    desc.layers = 1;
    desc.format = static_cast<uint32_t>(fmt);
    desc.usage = usage;
    desc.stride = static_cast<uint32_t>(stride);
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_createFromHandle(
            &desc, h, AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE, &ahb) != 0 ||
        ahb == nullptr) {
        ALOGE("dst createFromHandle failed");
        return EGL_NO_IMAGE_KHR;
    }
    EGLClientBuffer client = eglGetNativeClientBufferANDROID_(ahb);
    if (!client) {
        AHardwareBuffer_release(ahb);
        return EGL_NO_IMAGE_KHR;
    }
    const EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR img = eglCreateImageKHR_(mDpy, EGL_NO_CONTEXT,
                                         EGL_NATIVE_BUFFER_ANDROID, client, attrs);
    if (img != EGL_NO_IMAGE_KHR) {
        *keepAlive = ahb;                // released by composite() after use
    } else {
        AHardwareBuffer_release(ahb);
    }
    return img;
}

bool GpuCompositor::composite(AHardwareBuffer* src, int srcAcquireFence,
                              buffer_handle_t dst,
                              int width, int height, int stride, int dstFormat,
                              uint64_t dstUsage, int* outFence) {
    if (outFence) *outFence = -1;
    if (dstFormat != kFmtRGBA8888 && dstFormat != kFmtRGBX8888) {
        return false;  // YUV etc. -> CPU fallback
    }
    std::lock_guard<std::mutex> lock(mLock);
    if (!ensureInit()) return false;

    if (!eglMakeCurrent(mDpy, mPbuf, mPbuf, mCtx)) {
        ALOGE("composite: eglMakeCurrent failed (0x%x)", eglGetError());
        return false;
    }

    // Producer's acquire fence: make the GPU wait for the producer's render
    // to land before we sample src. GPU-side wait when native fences work,
    // CPU wait otherwise.
    if (srcAcquireFence >= 0) {
        bool waited = false;
        if (mNativeFence) {
            int fd = ::dup(srcAcquireFence);   // the sync object takes ownership
            const EGLint attrs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE };
            EGLSyncKHR s = eglCreateSyncKHR_(mDpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
            if (s != EGL_NO_SYNC_KHR) {
                waited = (eglWaitSyncKHR_(mDpy, s, 0) == EGL_TRUE);
                eglDestroySyncKHR_(mDpy, s);
            } else {
                ::close(fd);
            }
        }
        if (!waited) sync_wait(srcAcquireFence, /*timeout ms*/ 100);
    }

    EGLImageKHR srcImg = imageFromAhb(src);
    if (srcImg == EGL_NO_IMAGE_KHR) {
        ALOGE("composite: src EGLImage failed");
        eglMakeCurrent(mDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return false;
    }
    void* dstGb = nullptr;
    EGLImageKHR dstImg = imageFromHandle(dst, width, height, dstFormat,
                                         dstUsage, stride, &dstGb);
    if (dstImg == EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR_(mDpy, srcImg);
        ALOGE("composite: dst EGLImage failed");
        eglMakeCurrent(mDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return false;
    }

    // Source as sampled texture.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mSrcTex);
    glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, srcImg);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Destination as an FBO color attachment via a RENDERBUFFER. Rendering into
    // an imported gralloc EGLImage must go through a renderbuffer
    // (glEGLImageTargetRenderbufferStorageOES) — texture attachment is for
    // sampling and is not color-renderable on most drivers.
    GLuint dstRb = 0;
    glGenRenderbuffers(1, &dstRb);
    glBindRenderbuffer(GL_RENDERBUFFER, dstRb);
    glEGLImageTargetRenderbufferStorageOES_(GL_RENDERBUFFER, dstImg);
    glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, dstRb);
    GLenum fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    bool fboOk = (fbs == GL_FRAMEBUFFER_COMPLETE);
    if (!fboOk) {
        ALOGE("composite: FBO incomplete status=0x%x glErr=0x%x", fbs, glGetError());
    }

    bool result = false;
    if (fboOk) {
        static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
        glViewport(0, 0, width, height);
        glUseProgram(mProg);
        glEnableVertexAttribArray(mAttrPos);
        glVertexAttribPointer(mAttrPos, 2, GL_FLOAT, GL_FALSE, 0, quad);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mSrcTex);
        glUniform1i(mUniTex, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        result = (glGetError() == GL_NO_ERROR);
        // Completion: hand back a fence instead of stalling here. The same
        // fence covers the read of src and the write of dst.
        int fence = -1;
        if (result && mNativeFence && outFence) {
            EGLSyncKHR s = eglCreateSyncKHR_(mDpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
            if (s != EGL_NO_SYNC_KHR) {
                glFlush();   // the fence is only created once the command stream is submitted
                fence = eglDupNativeFenceFDANDROID_(mDpy, s);
                eglDestroySyncKHR_(mDpy, s);
            }
        }
        if (fence >= 0) {
            *outFence = fence;
        } else {
            glFinish();  // no fence available: complete synchronously (old behavior)
        }
    } else {
        ALOGE("composite: FBO incomplete");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteRenderbuffers(1, &dstRb);
    eglDestroyImageKHR_(mDpy, dstImg);
    eglDestroyImageKHR_(mDpy, srcImg);
    if (dstGb) AHardwareBuffer_release(reinterpret_cast<AHardwareBuffer*>(dstGb));

    // Release the context so the next capture request (possibly a different
    // binder thread) can acquire it.
    eglMakeCurrent(mDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return result;
}

}  // namespace virtualcamera

#endif  // VCAM_GPU_COMPOSITOR
