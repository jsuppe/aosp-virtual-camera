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

// ---- YUV packed-plane shaders (BT.601 limited range, same coefficients as
// FrameFiller::rgbaToYuv: Y = 66R+129G+25B >>8 +16, U = -38R-74G+112B, V = 112R-94G-18B).
const char* kVertYuv =
        "attribute vec2 aPos;\n"
        "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
// Each fragment = one RGBA texel = 4 consecutive Y bytes of one row.
const char* kFragY =
        "precision highp float;\n"
        "uniform sampler2D uTex;\n"
        "uniform vec2 uSize;\n"            // source width, height in pixels
        "float luma(vec2 uv) {\n"
        "  vec3 c = texture2D(uTex, uv).rgb;\n"
        "  return dot(c, vec3(66.0, 129.0, 25.0)) / 256.0 + 16.0 / 255.0;\n"
        "}\n"
        "void main() {\n"
        "  float x0 = (gl_FragCoord.x - 0.5) * 4.0;\n"      // first source column
        "  float v = gl_FragCoord.y / uSize.y;\n"
        "  gl_FragColor = vec4(luma(vec2((x0 + 0.5) / uSize.x, v)),\n"
        "                      luma(vec2((x0 + 1.5) / uSize.x, v)),\n"
        "                      luma(vec2((x0 + 2.5) / uSize.x, v)),\n"
        "                      luma(vec2((x0 + 3.5) / uSize.x, v)));\n"
        "}\n";
// Each fragment = U0 V0 U1 V1 for two chroma samples (4 source columns, 2 rows).
// Sampling at the centre of each 2x2 block with LINEAR filtering averages it.
const char* kFragUv =
        "precision highp float;\n"
        "uniform sampler2D uTex;\n"
        "uniform vec2 uSize;\n"
        "uniform float uSwap;\n"          // 1.0 -> V first (NV21)
        "vec2 chroma(vec2 uv) {\n"
        "  vec3 c = texture2D(uTex, uv).rgb;\n"
        "  float u = dot(c, vec3(-38.0, -74.0, 112.0)) / 256.0 + 128.0 / 255.0;\n"
        "  float v = dot(c, vec3(112.0, -94.0, -18.0)) / 256.0 + 128.0 / 255.0;\n"
        "  return uSwap > 0.5 ? vec2(v, u) : vec2(u, v);\n"
        "}\n"
        "void main() {\n"
        "  float x0 = (gl_FragCoord.x - 0.5) * 4.0;\n"
        "  float y = (gl_FragCoord.y - 0.5) * 2.0 + 1.0;\n"   // centre of the 2-row block
        "  vec2 c0 = chroma(vec2((x0 + 1.0) / uSize.x, y / uSize.y));\n"
        "  vec2 c1 = chroma(vec2((x0 + 3.0) / uSize.x, y / uSize.y));\n"
        "  gl_FragColor = vec4(c0, c1);\n"
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

GLuint GpuCompositor::buildProgram(const char* vert, const char* frag) {
    GLuint vs = compile(GL_VERTEX_SHADER, vert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, frag);
    if (!vs || !fs) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        ALOGE("program link failed: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

void GpuCompositor::releaseYuvTargets() {
    if (mYRb) { glDeleteRenderbuffers(1, &mYRb); mYRb = 0; }
    if (mUvRb) { glDeleteRenderbuffers(1, &mUvRb); mUvRb = 0; }
    if (mYImg != EGL_NO_IMAGE_KHR) { eglDestroyImageKHR_(mDpy, mYImg); mYImg = EGL_NO_IMAGE_KHR; }
    if (mUvImg != EGL_NO_IMAGE_KHR) { eglDestroyImageKHR_(mDpy, mUvImg); mUvImg = EGL_NO_IMAGE_KHR; }
    if (mYPack) { AHardwareBuffer_release(mYPack); mYPack = nullptr; }
    if (mUvPack) { AHardwareBuffer_release(mUvPack); mUvPack = nullptr; }
    mYuvW = mYuvH = 0;
}

bool GpuCompositor::ensureYuvTargets(int width, int height) {
    if (mYPack && mYuvW == width && mYuvH == height) return true;
    releaseYuvTargets();
    if (!mProgY) {
        mProgY = buildProgram(kVertYuv, kFragY);
        mProgUv = buildProgram(kVertYuv, kFragUv);
        if (!mProgY || !mProgUv) return false;
        mYAttrPos = glGetAttribLocation(mProgY, "aPos");
        mYUniTex = glGetUniformLocation(mProgY, "uTex");
        mYUniSize = glGetUniformLocation(mProgY, "uSize");
        mUvAttrPos = glGetAttribLocation(mProgUv, "aPos");
        mUvUniTex = glGetUniformLocation(mProgUv, "uTex");
        mUvUniSize = glGetUniformLocation(mProgUv, "uSize");
        mUvUniSwap = glGetUniformLocation(mProgUv, "uSwap");
    }
    auto alloc = [](int w, int h) -> AHardwareBuffer* {
        AHardwareBuffer_Desc d = {};
        d.width = static_cast<uint32_t>(w);
        d.height = static_cast<uint32_t>(h);
        d.layers = 1;
        d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        d.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
        AHardwareBuffer* b = nullptr;
        return AHardwareBuffer_allocate(&d, &b) == 0 ? b : nullptr;
    };
    mYPack = alloc(width / 4, height);
    mUvPack = alloc(width / 4, height / 2);
    if (!mYPack || !mUvPack) {
        ALOGE("yuv targets: allocation failed");
        releaseYuvTargets();
        return false;
    }
    mYImg = imageFromAhb(mYPack);
    mUvImg = imageFromAhb(mUvPack);
    if (mYImg == EGL_NO_IMAGE_KHR || mUvImg == EGL_NO_IMAGE_KHR) {
        ALOGE("yuv targets: EGLImage failed");
        releaseYuvTargets();
        return false;
    }
    glGenRenderbuffers(1, &mYRb);
    glBindRenderbuffer(GL_RENDERBUFFER, mYRb);
    glEGLImageTargetRenderbufferStorageOES_(GL_RENDERBUFFER, mYImg);
    glGenRenderbuffers(1, &mUvRb);
    glBindRenderbuffer(GL_RENDERBUFFER, mUvRb);
    glEGLImageTargetRenderbufferStorageOES_(GL_RENDERBUFFER, mUvImg);
    mYuvW = width;
    mYuvH = height;
    ALOGI("yuv targets ready: packed Y %dx%d, UV %dx%d (RGBA texels)",
          width / 4, height, width / 4, height / 2);
    return true;
}

bool GpuCompositor::compositeToYcbcr(AHardwareBuffer* src, int srcAcquireFence,
                                     const YCbCrBuffer& dst, int width, int height) {
    if (!dst.y || !dst.cb || !dst.cr || (width % 4) != 0 || (height % 2) != 0) return false;
    std::lock_guard<std::mutex> lock(mLock);
    if (!ensureInit()) return false;
    if (!eglMakeCurrent(mDpy, mPbuf, mPbuf, mCtx)) {
        ALOGE("compositeToYcbcr: eglMakeCurrent failed (0x%x)", eglGetError());
        return false;
    }
    struct Release {   // always drop the context on the way out
        EGLDisplay d; ~Release() { eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); }
    } release{mDpy};

    if (!ensureYuvTargets(width, height)) return false;

    // Producer's acquire fence: GPU-side wait (same as composite()).
    if (srcAcquireFence >= 0) {
        bool waited = false;
        if (mNativeFence) {
            int fd = ::dup(srcAcquireFence);
            const EGLint attrs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE };
            EGLSyncKHR s = eglCreateSyncKHR_(mDpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
            if (s != EGL_NO_SYNC_KHR) {
                waited = (eglWaitSyncKHR_(mDpy, s, 0) == EGL_TRUE);
                eglDestroySyncKHR_(mDpy, s);
            } else {
                ::close(fd);
            }
        }
        if (!waited) sync_wait(srcAcquireFence, 100);
    }

    EGLImageKHR srcImg = imageFromAhb(src);
    if (srcImg == EGL_NO_IMAGE_KHR) {
        ALOGE("compositeToYcbcr: src EGLImage failed");
        return false;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mSrcTex);
    glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, srcImg);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    const bool nv21 = (dst.chroma_step == 2 &&
                       static_cast<uint8_t*>(dst.cb) == static_cast<uint8_t*>(dst.cr) + 1);
    bool ok = true;
    // Pass 1: packed Y.
    glBindFramebuffer(GL_FRAMEBUFFER, mFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, mYRb);
    ok = ok && (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    if (ok) {
        glViewport(0, 0, width / 4, height);
        glUseProgram(mProgY);
        glUniform1i(mYUniTex, 0);
        glUniform2f(mYUniSize, static_cast<float>(width), static_cast<float>(height));
        glEnableVertexAttribArray(mYAttrPos);
        glVertexAttribPointer(mYAttrPos, 2, GL_FLOAT, GL_FALSE, 0, quad);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    // Pass 2: packed interleaved UV.
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, mUvRb);
    ok = ok && (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    if (ok) {
        glViewport(0, 0, width / 4, height / 2);
        glUseProgram(mProgUv);
        glUniform1i(mUvUniTex, 0);
        glUniform2f(mUvUniSize, static_cast<float>(width), static_cast<float>(height));
        glUniform1f(mUvUniSwap, nv21 ? 1.0f : 0.0f);
        glEnableVertexAttribArray(mUvAttrPos);
        glVertexAttribPointer(mUvAttrPos, 2, GL_FLOAT, GL_FALSE, 0, quad);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glFinish();   // the CPU copy below needs the GPU result
    ok = ok && (glGetError() == GL_NO_ERROR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    eglDestroyImageKHR_(mDpy, srcImg);
    if (!ok) {
        ALOGE("compositeToYcbcr: GL pass failed");
        return false;
    }

    // Copy packed planes into the framework's YUV buffer.
    AHardwareBuffer_Desc yd, uvd;
    AHardwareBuffer_describe(mYPack, &yd);
    AHardwareBuffer_describe(mUvPack, &uvd);
    void* yp = nullptr; void* uvp = nullptr;
    if (AHardwareBuffer_lock(mYPack, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &yp) != 0 ||
        AHardwareBuffer_lock(mUvPack, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &uvp) != 0) {
        ALOGE("compositeToYcbcr: packed plane lock failed");
        if (yp) AHardwareBuffer_unlock(mYPack, nullptr);
        return false;
    }
    const size_t yRow = static_cast<size_t>(yd.stride) * 4;
    const size_t uvRow = static_cast<size_t>(uvd.stride) * 4;
    const uint8_t* ys = static_cast<const uint8_t*>(yp);
    const uint8_t* uvs = static_cast<const uint8_t*>(uvp);
    uint8_t* dy = static_cast<uint8_t*>(dst.y);
    for (int r = 0; r < height; r++) {
        memcpy(dy + static_cast<size_t>(r) * dst.ystride, ys + static_cast<size_t>(r) * yRow, width);
    }
    const int chromaH = height / 2;
    if (dst.chroma_step == 2) {
        // NV12 (cb first) or NV21 (cr first, handled by uSwap): one interleaved plane.
        uint8_t* base = static_cast<uint8_t*>(nv21 ? dst.cr : dst.cb);
        for (int r = 0; r < chromaH; r++) {
            memcpy(base + static_cast<size_t>(r) * dst.cstride, uvs + static_cast<size_t>(r) * uvRow, width);
        }
    } else {
        // Planar: de-interleave U/V bytes.
        uint8_t* dcb = static_cast<uint8_t*>(dst.cb);
        uint8_t* dcr = static_cast<uint8_t*>(dst.cr);
        const int chromaW = width / 2;
        for (int r = 0; r < chromaH; r++) {
            const uint8_t* s = uvs + static_cast<size_t>(r) * uvRow;
            uint8_t* cb = dcb + static_cast<size_t>(r) * dst.cstride;
            uint8_t* cr = dcr + static_cast<size_t>(r) * dst.cstride;
            for (int c = 0; c < chromaW; c++) { cb[c] = s[2 * c]; cr[c] = s[2 * c + 1]; }
        }
    }
    AHardwareBuffer_unlock(mYPack, nullptr);
    AHardwareBuffer_unlock(mUvPack, nullptr);
    return true;
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
