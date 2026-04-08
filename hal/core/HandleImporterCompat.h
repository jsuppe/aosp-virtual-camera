/*
 * HandleImporterCompat - Compatibility shim for HandleImporter across Android versions
 *
 * A13 (camera.common@1.0-helper) uses IMapper::Rect and YCbCrLayout.
 * A15 (camera.common-helper) uses android::Rect and android_ycbcr.
 *
 * This header provides unified types and a lockYCbCr wrapper.
 */
#pragma once

#include <HandleImporter.h>

// VCAM_COMPAT_A13 is set via cflags by integrate.sh for A13 builds.
// If not set, assume A15+ (AIDL camera.common-helper).
#ifdef VCAM_COMPAT_A13
    // A13 (HIDL camera.common@1.0-helper) — uses V2.0 IMapper types
    #include <android/hardware/graphics/mapper/2.0/IMapper.h>
#else
    // A15+ (AIDL camera.common-helper) — uses android::Rect and android_ycbcr
    #include <ui/Rect.h>
#endif

namespace virtualcamera {

using HandleImporter = ::android::hardware::camera::common::V1_0::helper::HandleImporter;

/**
 * YCbCr buffer layout — normalized across versions.
 * Both A13's YCbCrLayout and A15's android_ycbcr have the same fields.
 */
struct YCbCrBuffer {
    void* y = nullptr;
    void* cb = nullptr;
    void* cr = nullptr;
    int ystride = 0;
    int cstride = 0;
    int chroma_step = 0;
};

/**
 * Lock a gralloc buffer as YCbCr, returning a unified YCbCrBuffer.
 */
inline YCbCrBuffer lockYCbCrCompat(HandleImporter& importer,
                                    buffer_handle_t handle,
                                    uint64_t usage,
                                    int width, int height) {
    YCbCrBuffer result;

#ifdef VCAM_COMPAT_A13
    using IMapper = ::android::hardware::graphics::mapper::V2_0::IMapper;
    IMapper::Rect region{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
    auto layout = importer.lockYCbCr(handle, usage, region);
    result.y = layout.y;
    result.cb = layout.cb;
    result.cr = layout.cr;
    result.ystride = layout.yStride;
    result.cstride = layout.cStride;
    result.chroma_step = layout.chromaStep;
#else
    ::android::Rect region(0, 0, width, height);
    android_ycbcr ycbcr = importer.lockYCbCr(handle, usage, region);
    result.y = ycbcr.y;
    result.cb = ycbcr.cb;
    result.cr = ycbcr.cr;
    result.ystride = ycbcr.ystride;
    result.cstride = ycbcr.cstride;
    result.chroma_step = ycbcr.chroma_step;
#endif

    return result;
}

}  // namespace virtualcamera
