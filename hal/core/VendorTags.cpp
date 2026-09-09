/*
 * VendorTags implementation.
 */

#define LOG_TAG "VCamVendorTags"

#include "VendorTags.h"

#include <log/log.h>
#include <system/camera_metadata.h>
#include <system/camera_vendor_tags.h>

extern "C" int set_camera_metadata_vendor_ops(const vendor_tag_ops_t* ops);

namespace virtualcamera {

namespace {

constexpr const char* kSectionName = "com.virtualcamera";

const VendorTags::Def kTags[] = {
    { VendorTags::kProducerTimestampNs, "producerTimestampNs", TYPE_INT64 },
};
constexpr size_t kTagCount = sizeof(kTags) / sizeof(kTags[0]);

const VendorTags::Def* find(uint32_t tag) {
    for (const auto& d : kTags) {
        if (d.id == tag) return &d;
    }
    return nullptr;
}

int getTagCount(const vendor_tag_ops_t*) { return static_cast<int>(kTagCount); }
void getAllTags(const vendor_tag_ops_t*, uint32_t* out) {
    for (size_t i = 0; i < kTagCount; i++) out[i] = kTags[i].id;
}
const char* getSectionName(const vendor_tag_ops_t*, uint32_t tag) {
    return find(tag) ? kSectionName : nullptr;
}
const char* getTagName(const vendor_tag_ops_t*, uint32_t tag) {
    const auto* d = find(tag);
    return d ? d->name : nullptr;
}
int getTagType(const vendor_tag_ops_t*, uint32_t tag) {
    const auto* d = find(tag);
    return d ? d->type : -1;
}

}  // namespace

const char* VendorTags::sectionName() { return kSectionName; }

const VendorTags::Def* VendorTags::tags(size_t* count) {
    if (count) *count = kTagCount;
    return kTags;
}

void VendorTags::installMetadataOps() {
    static vendor_tag_ops_t ops = {};
    static bool installed = false;
    if (installed) return;
    ops.get_tag_count = getTagCount;
    ops.get_all_tags = getAllTags;
    ops.get_section_name = getSectionName;
    ops.get_tag_name = getTagName;
    ops.get_tag_type = getTagType;
    int rc = set_camera_metadata_vendor_ops(&ops);
    installed = (rc == 0);
    ALOGI("vendor tag ops %s (%zu tags in %s)", installed ? "installed" : "FAILED",
          kTagCount, kSectionName);
}

}  // namespace virtualcamera
