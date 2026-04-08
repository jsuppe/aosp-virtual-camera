/*
 * GpuRenderer - Renders test pattern directly to NV12 + RGBA via GPU shaders
 *
 * The test pattern (rotating wave with grid) is computed entirely on the GPU.
 * Two output paths, both from the same shader math:
 *   - NV12 for the HAL (Y plane + UV plane, two render passes)
 *   - RGBA for the display surface
 *
 * Zero CPU pixel work. Zero format conversion.
 */
#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cstdint>

class GpuRenderer {
public:
    GpuRenderer() = default;
    ~GpuRenderer();

    bool initialize(int width, int height);
    void shutdown();

    /**
     * Render one frame. Outputs both RGBA (for display) and NV12 (for HAL).
     * frameIndex: animation frame counter
     */
    void renderFrame(int frameIndex, uint8_t* rgbaOut, uint8_t* nv12Out);

    bool isInitialized() const { return mInitialized; }

private:
    GLuint createShader(GLenum type, const char* source);
    GLuint createProgram(const char* vertSrc, const char* fragSrc);
    void renderPass(GLuint program, GLuint fbo, int vpWidth, int vpHeight, int frame);

    int mWidth = 0;
    int mHeight = 0;
    bool mInitialized = false;

    EGLDisplay mDisplay = EGL_NO_DISPLAY;
    EGLContext mContext = EGL_NO_CONTEXT;
    EGLSurface mSurface = EGL_NO_SURFACE;

    // FBOs and textures for NV12 output
    GLuint mYFbo = 0, mYTex = 0;     // Y plane: R8 full-res
    GLuint mUvFbo = 0, mUvTex = 0;   // UV plane: RG8 half-res
    GLuint mRgbaFbo = 0, mRgbaTex = 0; // RGBA: full-res

    // Shader programs (same pattern math, different color output)
    GLuint mYProgram = 0;    // outputs Y
    GLuint mUvProgram = 0;   // outputs CbCr
    GLuint mRgbaProgram = 0; // outputs RGBA

    GLuint mQuadVao = 0, mQuadVbo = 0;
};
