/*
 * GpuRenderer - Test pattern rendered directly as NV12 + RGBA on GPU
 *
 * The rotating wave pattern is computed in the fragment shader. Three programs
 * share the same pattern math but output different color spaces:
 *   Y program:    BT.601 luminance → R8 texture (full res)
 *   UV program:   BT.601 chrominance → RG8 texture (half res)
 *   RGBA program: standard RGB → RGBA8 texture (for display)
 */

#include "gpu_renderer.h"
#include <android/log.h>
#include <cstring>

#define LOG_TAG "GpuRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const float kQuad[] = {
    -1.f, -1.f, 0.f, 0.f,
     1.f, -1.f, 1.f, 0.f,
    -1.f,  1.f, 0.f, 1.f,
     1.f,  1.f, 1.f, 1.f,
};

static const char* kVert = R"(#version 300 es
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)";

// Shared pattern math as a GLSL function.
// Returns linear RGB [0,1] for the test pattern at given UV and time.
#define PATTERN_FUNC R"(
vec3 pattern(vec2 uv, float time) {
    vec2 p = uv - 0.5;
    float angle = time;
    float ca = cos(angle), sa = sin(angle);
    vec2 r = vec2(p.x*ca - p.y*sa, p.x*sa + p.y*ca);
    float dist = length(r);
    float wave = sin(dist * 10.0 - time * 2.0) * 0.5 + 0.5;
    vec3 rgb = vec3(200.0*wave + 55.0, 150.0*wave + 50.0, 50.0*wave + 20.0) / 255.0;
    // Grid lines
    vec2 px = uv * vec2(WIDTH, HEIGHT);
    if (mod(px.x, 64.0) < 2.0 || mod(px.y, 64.0) < 2.0) rgb = vec3(1.0);
    return rgb;
}
)"

// Y plane shader: outputs BT.601 luminance
static const char* kYFrag = R"(#version 300 es
precision mediump float;
in vec2 vUV;
uniform float uTime;
uniform float WIDTH;
uniform float HEIGHT;
)" PATTERN_FUNC R"(
out vec4 fragColor;
void main() {
    vec3 rgb = pattern(vUV, uTime);
    float y = 0.257*rgb.r + 0.504*rgb.g + 0.098*rgb.b + 0.0625;
    fragColor = vec4(y, 0.0, 0.0, 1.0);
}
)";

// UV plane shader: outputs BT.601 Cb (R) and Cr (G) at half-res
static const char* kUvFrag = R"(#version 300 es
precision mediump float;
in vec2 vUV;
uniform float uTime;
uniform float WIDTH;
uniform float HEIGHT;
)" PATTERN_FUNC R"(
out vec4 fragColor;
void main() {
    vec3 rgb = pattern(vUV, uTime);
    float cb = -0.148*rgb.r - 0.291*rgb.g + 0.439*rgb.b + 0.5;
    float cr =  0.439*rgb.r - 0.368*rgb.g - 0.071*rgb.b + 0.5;
    fragColor = vec4(cb, cr, 0.0, 1.0);
}
)";

// RGBA shader: outputs standard RGB for display
static const char* kRgbaFrag = R"(#version 300 es
precision mediump float;
in vec2 vUV;
uniform float uTime;
uniform float WIDTH;
uniform float HEIGHT;
)" PATTERN_FUNC R"(
out vec4 fragColor;
void main() {
    fragColor = vec4(pattern(vUV, uTime), 1.0);
}
)";

GpuRenderer::~GpuRenderer() { shutdown(); }

GLuint GpuRenderer::createShader(GLenum type, const char* source) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("Shader error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint GpuRenderer::createProgram(const char* vertSrc, const char* fragSrc) {
    GLuint v = createShader(GL_VERTEX_SHADER, vertSrc);
    GLuint f = createShader(GL_FRAGMENT_SHADER, fragSrc);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("Link error: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

bool GpuRenderer::initialize(int width, int height) {
    mWidth = width;
    mHeight = height;

    // Offscreen EGL context
    mDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(mDisplay, nullptr, nullptr);

    EGLint cfgAttrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n;
    eglChooseConfig(mDisplay, cfgAttrs, &cfg, 1, &n);
    if (n == 0) { LOGE("No EGL config"); return false; }

    EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    mContext = eglCreateContext(mDisplay, cfg, EGL_NO_CONTEXT, ctxAttrs);
    EGLint pbAttrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    mSurface = eglCreatePbufferSurface(mDisplay, cfg, pbAttrs);
    eglMakeCurrent(mDisplay, mSurface, mSurface, mContext);

    LOGI("GL: %s", glGetString(GL_VERSION));

    // Compile programs
    mYProgram = createProgram(kVert, kYFrag);
    mUvProgram = createProgram(kVert, kUvFrag);
    mRgbaProgram = createProgram(kVert, kRgbaFrag);
    if (!mYProgram || !mUvProgram || !mRgbaProgram) {
        LOGE("Shader compilation failed");
        shutdown();
        return false;
    }

    // Helper: create texture + FBO
    auto makeFbo = [](GLuint* tex, GLuint* fbo, GLenum fmt, int w, int h) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0,
                     (fmt == GL_RGBA8 ? GL_RGBA : (fmt == GL_RG8 ? GL_RG : GL_RED)),
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glGenFramebuffers(1, fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    };

    makeFbo(&mYTex, &mYFbo, GL_R8, width, height);
    makeFbo(&mUvTex, &mUvFbo, GL_RG8, width / 2, height / 2);
    makeFbo(&mRgbaTex, &mRgbaFbo, GL_RGBA8, width, height);

    // Quad geometry
    glGenVertexArrays(1, &mQuadVao);
    glGenBuffers(1, &mQuadVbo);
    glBindVertexArray(mQuadVao);
    glBindBuffer(GL_ARRAY_BUFFER, mQuadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    mInitialized = true;
    LOGI("GPU renderer initialized: %dx%d", width, height);
    return true;
}

void GpuRenderer::shutdown() {
    if (mDisplay != EGL_NO_DISPLAY)
        eglMakeCurrent(mDisplay, mSurface, mSurface, mContext);

    auto del = [](GLuint* t, GLuint* f) {
        if (*t) { glDeleteTextures(1, t); *t = 0; }
        if (*f) { glDeleteFramebuffers(1, f); *f = 0; }
    };
    del(&mYTex, &mYFbo);
    del(&mUvTex, &mUvFbo);
    del(&mRgbaTex, &mRgbaFbo);

    if (mQuadVao) { glDeleteVertexArrays(1, &mQuadVao); mQuadVao = 0; }
    if (mQuadVbo) { glDeleteBuffers(1, &mQuadVbo); mQuadVbo = 0; }
    if (mYProgram) { glDeleteProgram(mYProgram); mYProgram = 0; }
    if (mUvProgram) { glDeleteProgram(mUvProgram); mUvProgram = 0; }
    if (mRgbaProgram) { glDeleteProgram(mRgbaProgram); mRgbaProgram = 0; }

    if (mDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (mSurface != EGL_NO_SURFACE) eglDestroySurface(mDisplay, mSurface);
        if (mContext != EGL_NO_CONTEXT) eglDestroyContext(mDisplay, mContext);
        eglTerminate(mDisplay);
    }
    mDisplay = EGL_NO_DISPLAY;
    mContext = EGL_NO_CONTEXT;
    mSurface = EGL_NO_SURFACE;
    mInitialized = false;
}

void GpuRenderer::renderPass(GLuint program, GLuint fbo, int vpW, int vpH, int frame) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, vpW, vpH);
    glUseProgram(program);
    glUniform1f(glGetUniformLocation(program, "uTime"), frame * 0.05f);
    glUniform1f(glGetUniformLocation(program, "WIDTH"), (float)mWidth);
    glUniform1f(glGetUniformLocation(program, "HEIGHT"), (float)mHeight);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void GpuRenderer::renderFrame(int frameIndex, uint8_t* rgbaOut, uint8_t* nv12Out) {
    if (!mInitialized) return;

    eglMakeCurrent(mDisplay, mSurface, mSurface, mContext);
    glBindVertexArray(mQuadVao);

    // Pass 1: Y plane (full res)
    renderPass(mYProgram, mYFbo, mWidth, mHeight, frameIndex);
    glReadPixels(0, 0, mWidth, mHeight, GL_RED, GL_UNSIGNED_BYTE, nv12Out);

    // Pass 2: UV plane (half res)
    renderPass(mUvProgram, mUvFbo, mWidth / 2, mHeight / 2, frameIndex);
    glReadPixels(0, 0, mWidth / 2, mHeight / 2, GL_RG, GL_UNSIGNED_BYTE,
                 nv12Out + mWidth * mHeight);

    // Pass 3: RGBA for display
    renderPass(mRgbaProgram, mRgbaFbo, mWidth, mHeight, frameIndex);
    glReadPixels(0, 0, mWidth, mHeight, GL_RGBA, GL_UNSIGNED_BYTE, rgbaOut);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
