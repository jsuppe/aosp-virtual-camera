/*
 * VendorTags - the virtual camera's vendor metadata section.
 *
 * Section "com.virtualcamera" (VENDOR_SECTION 0x8000). Apps read the tags with
 *   new CaptureResult.Key<>("com.virtualcamera.<tagName>", <Type>.class)
 * once the provider has published the section via ICameraProvider.getVendorTags().
 *
 * Tags:
 *   producerTimestampNs (INT64) — the producer's queue timestamp
 *     (CLOCK_MONOTONIC, from the BufferQueue) of the frame this result carries.
 *     SENSOR_TIMESTAMP stays the HAL's paced capture slot (unique and
 *     monotonic per result, which recorders need); this tag is what lets a
 *     consumer measure producer->consumer latency honestly, even when the
 *     same producer frame is delivered in several capture results.
 *
 * No AIDL dependencies: the provider adapts the table to VendorTagSection.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace virtualcamera {

struct VendorTags {
    struct Def {
        uint32_t id;
        const char* name;
        int type;   // camera_metadata TYPE_*
    };

    static constexpr uint32_t kSection = 0x8000;                    // VENDOR_SECTION
    static constexpr uint32_t kProducerTimestampNs = (kSection << 16) | 0;

    static const char* sectionName();
    static const Def* tags(size_t* count);

    /** Register the table with libcamera_metadata so the HAL can add these
     *  tags to results (add_camera_metadata_entry validates the type). */
    static void installMetadataOps();
};

}  // namespace virtualcamera
