/*
 * MetadataBuilder - Camera metadata construction utilities
 */

#define LOG_TAG "VCamMetadataBuilder"

#include "MetadataBuilder.h"
#include "VendorTags.h"

#include <log/log.h>
#include <system/camera_metadata.h>
#include <system/graphics.h>

#include <algorithm>
#include <cstring>
#include <vector>
#include <map>
#include <array>
#include <set>
#include <tuple>

namespace virtualcamera {

std::vector<uint8_t> MetadataBuilder::buildCameraCharacteristics(const ProducerCapabilities* capabilities) {
    const auto fallback = ProducerCapabilities::diagnostic();
    const auto& caps = capabilities ? *capabilities : fallback;
    if (!caps.valid()) return {};
    int arrayWidth=0,arrayHeight=0,maxPixels=0; bool jpeg=false;
    for(const auto& m:caps.modes) { arrayWidth=std::max(arrayWidth,m.width);arrayHeight=std::max(arrayHeight,m.height);
        maxPixels=std::max(maxPixels,m.width*m.height);
        for(int f:m.formats) if(f==0x21)jpeg=true; }
    // Create camera characteristics with enough space for all entries
    camera_metadata_t* meta = allocate_camera_metadata(128, 32768);

    // Required characteristics for a basic camera
    uint8_t facing = caps.facing;
    add_camera_metadata_entry(meta, ANDROID_LENS_FACING, &facing, 1);

    int32_t orientation = caps.orientation;
    add_camera_metadata_entry(meta, ANDROID_SENSOR_ORIENTATION, &orientation, 1);

    // Only declared, pipeline-supported mode outputs. Collapse duplicates using fastest declared rate.
    std::map<std::tuple<int,int,int>,int> outputs;
    for(const auto& m:caps.modes)for(int f:m.formats) {
        auto key=std::make_tuple(f,m.width,m.height); outputs[key]=std::max(outputs[key],m.maxFps);
    }
    std::vector<int32_t> streamConfigs;
    std::vector<int64_t> frameDurations,stallDurations;
    for(const auto& entry:outputs) {
        auto [f,w,h]=entry.first;
        for(int n:std::array<int,4>{f,w,h,int(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT)})streamConfigs.push_back(n);
        for(int64_t n:std::array<int64_t,4>{f,w,h,(1000000000LL+entry.second-1)/entry.second})frameDurations.push_back(n);
        for(int64_t n:std::array<int64_t,4>{f,w,h,f==0x21?100000000:0})stallDurations.push_back(n);
    }
    add_camera_metadata_entry(meta,ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,streamConfigs.data(),streamConfigs.size());
    add_camera_metadata_entry(meta,ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,frameDurations.data(),frameDurations.size());
    add_camera_metadata_entry(meta,ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,stallDurations.data(),stallDurations.size());
    // Hardware level
    uint8_t hwLevel = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL;
    add_camera_metadata_entry(meta, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &hwLevel, 1);

    // Available capabilities (required!)
    const uint8_t availableCapabilities[] = {
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE,
    };
    add_camera_metadata_entry(meta, ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
                              availableCapabilities, jpeg ? sizeof(availableCapabilities) : 0);

    // Partial result count (we send single complete result)
    int32_t partialResultCount = 1;
    add_camera_metadata_entry(meta, ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
                              &partialResultCount, 1);

    // 3A control: we have no 3A, so OFF and AUTO behave identically, but the
    // key set must be complete for BACKWARD_COMPATIBLE (VTS checks it).
    const uint8_t controlModes[] = { ANDROID_CONTROL_MODE_OFF, ANDROID_CONTROL_MODE_AUTO };
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AVAILABLE_MODES, controlModes, 2);
    const uint8_t aeModes[] = { ANDROID_CONTROL_AE_MODE_ON };
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AE_AVAILABLE_MODES, aeModes, 1);
    const uint8_t awbModes[] = { ANDROID_CONTROL_AWB_MODE_AUTO };
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AWB_AVAILABLE_MODES, awbModes, 1);
    const uint8_t afModes[] = { ANDROID_CONTROL_AF_MODE_OFF };
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AF_AVAILABLE_MODES, afModes, 1);

    uint8_t croppingType = ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY;
    add_camera_metadata_entry(meta, ANDROID_SCALER_CROPPING_TYPE, &croppingType, 1);

    // JPEG (BLOB) output
    int32_t jpegMaxSize = maxPixels * 4 + 65536;
    if (jpeg) {
    add_camera_metadata_entry(meta, ANDROID_JPEG_MAX_SIZE, &jpegMaxSize, 1);
    const int32_t thumbSizes[] = { 0, 0 };   // no thumbnails
    add_camera_metadata_entry(meta, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, thumbSizes, 2);
    }

    // Zoom ratio range (1.0x only - no zoom)
    const float zoomRange[] = {1.0f, 1.0f};
    add_camera_metadata_entry(meta, ANDROID_CONTROL_ZOOM_RATIO_RANGE, zoomRange, 2);

    // Active array size = max resolution (4K)
    const int32_t activeArray[] = {0, 0, arrayWidth, arrayHeight};
    add_camera_metadata_entry(meta, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, activeArray, 4);

    // Max digital zoom
    const float maxZoom = 1.0f;
    add_camera_metadata_entry(meta, ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &maxZoom, 1);

    // Scaler crop region = full sensor (4K)
    const int32_t cropRegion[] = {0, 0, arrayWidth, arrayHeight};
    add_camera_metadata_entry(meta, ANDROID_SCALER_CROP_REGION, cropRegion, 4);

    // AE available FPS ranges
    const auto rates=caps.fpsRanges();
    std::vector<int32_t> fpsRanges;
    for(const auto& r:rates){fpsRanges.push_back(r.first);fpsRanges.push_back(r.second);}
    add_camera_metadata_entry(meta,ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,fpsRanges.data(),fpsRanges.size());
    int32_t pixelSize[]={arrayWidth,arrayHeight};
    add_camera_metadata_entry(meta,ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,pixelSize,2);
    int minFps=120;for(const auto& m:caps.modes)minFps=std::min(minFps,m.minFps);
    int64_t maxDuration=1000000000LL/minFps;
    add_camera_metadata_entry(meta,ANDROID_SENSOR_INFO_MAX_FRAME_DURATION,&maxDuration,1);
    int32_t maxOutputs[]={0,std::min(2,caps.maxStreams),jpeg?1:0};
    add_camera_metadata_entry(meta,ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,maxOutputs,3);
    // Available request keys
    std::vector<int32_t> requestKeys = {
        ANDROID_CONTROL_MODE,
        ANDROID_CONTROL_AE_MODE,
        ANDROID_CONTROL_AWB_MODE,
        ANDROID_CONTROL_AE_TARGET_FPS_RANGE,   // drives the request-loop pacing
        ANDROID_CONTROL_ZOOM_RATIO,            // zoom keys: all four or none (VTS)
        ANDROID_JPEG_QUALITY,
        ANDROID_JPEG_ORIENTATION,
    };
    if(!jpeg) for(int k:{ANDROID_JPEG_QUALITY,ANDROID_JPEG_ORIENTATION})requestKeys.erase(std::remove(requestKeys.begin(),requestKeys.end(),k),requestKeys.end());
    add_camera_metadata_entry(meta, ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS,requestKeys.data(),requestKeys.size());

    // Available result keys
    const int32_t resultKeys[] = {
        ANDROID_SENSOR_TIMESTAMP,
        ANDROID_SENSOR_FRAME_DURATION,
        ANDROID_CONTROL_ZOOM_RATIO,
        ANDROID_SCALER_CROP_REGION,
        static_cast<int32_t>(VendorTags::kProducerTimestampNs),
    };
    add_camera_metadata_entry(meta, ANDROID_REQUEST_AVAILABLE_RESULT_KEYS,
                              resultKeys, sizeof(resultKeys)/sizeof(int32_t));

    // Available characteristics keys
    std::vector<int32_t> charKeys = {
        ANDROID_LENS_FACING,
        ANDROID_SENSOR_ORIENTATION,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
        ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
        ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
        ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
        ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
        ANDROID_CONTROL_AVAILABLE_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_MODES,
        ANDROID_CONTROL_AWB_AVAILABLE_MODES,
        ANDROID_CONTROL_AF_AVAILABLE_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
        ANDROID_CONTROL_ZOOM_RATIO_RANGE,
        ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
        ANDROID_SCALER_CROPPING_TYPE,
        ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
        ANDROID_JPEG_MAX_SIZE,
        ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
    };
    if(!jpeg)for(int k:{ANDROID_JPEG_MAX_SIZE,ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES})charKeys.erase(std::remove(charKeys.begin(),charKeys.end(),k),charKeys.end());
    for(int k:{ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,ANDROID_SENSOR_INFO_MAX_FRAME_DURATION,ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS})charKeys.push_back(k);
    add_camera_metadata_entry(meta, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,charKeys.data(),charKeys.size());

    // Serialize to byte vector
    size_t metaSize = get_camera_metadata_size(meta);
    std::vector<uint8_t> result(metaSize);
    memcpy(result.data(), meta, metaSize);
    free_camera_metadata(meta);

    ALOGI("Camera characteristics built: %zu bytes", metaSize);
    return result;
}

bool MetadataBuilder::isStreamCombinationSupported(const std::vector<StreamDesc>& streams,const ProducerCapabilities* capabilities) {
    const auto fallback=ProducerCapabilities::diagnostic();const auto& caps=capabilities?*capabilities:fallback;
    std::vector<ProducerCapabilities::Output> outputs;
    for(const auto& s:streams) {
        if(s.input||s.useCase!=0||s.rotation!=0)return false; // no unimplemented rotation promise
        outputs.push_back({s.format,s.width,s.height});
    }
    return caps.select(outputs)!=nullptr;
}

std::vector<uint8_t> MetadataBuilder::buildDefaultRequestSettings(const ProducerCapabilities* capabilities) {
    camera_metadata_t* meta = allocate_camera_metadata(10, 200);

    uint8_t controlMode = ANDROID_CONTROL_MODE_AUTO;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_MODE, &controlMode, 1);

    uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AE_MODE, &aeMode, 1);

    uint8_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
    auto range=capabilities ? capabilities->defaultRange() : std::make_pair(30,30);
    const int32_t fpsRange[] = { range.first, range.second };
    add_camera_metadata_entry(meta, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, fpsRange, 2);
    bool jpeg=capabilities==nullptr;
    if(capabilities)for(const auto& m:capabilities->modes)for(int f:m.formats)if(f==0x21)jpeg=true;
    if(jpeg) {
    uint8_t jpegQuality = 90;
    add_camera_metadata_entry(meta, ANDROID_JPEG_QUALITY, &jpegQuality, 1);
    int32_t jpegOrientation = 0;
    add_camera_metadata_entry(meta, ANDROID_JPEG_ORIENTATION, &jpegOrientation, 1);
    }

    float zoomRatio = 1.0f;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_ZOOM_RATIO, &zoomRatio, 1);

    size_t metaSize = get_camera_metadata_size(meta);
    std::vector<uint8_t> result(metaSize);
    memcpy(result.data(), meta, metaSize);
    free_camera_metadata(meta);

    return result;
}

std::vector<uint8_t> MetadataBuilder::buildResultMetadata(int64_t timestamp,
                                                          int64_t frameDurationNs,
                                                          int64_t producerTimestampNs, int width, int height) {
    camera_metadata_t* meta = allocate_camera_metadata(10, 200);

    int64_t ts = timestamp;
    add_camera_metadata_entry(meta, ANDROID_SENSOR_TIMESTAMP, &ts, 1);
    add_camera_metadata_entry(meta, ANDROID_SENSOR_FRAME_DURATION, &frameDurationNs, 1);
    if (producerTimestampNs > 0) {
        // Vendor tag (VendorTags.h): needs installMetadataOps() once per process.
        add_camera_metadata_entry(meta, VendorTags::kProducerTimestampNs,
                                  &producerTimestampNs, 1);
    }

    float zoomRatio = 1.0f;
    add_camera_metadata_entry(meta, ANDROID_CONTROL_ZOOM_RATIO, &zoomRatio, 1);

    int32_t cropRegion[] = {0, 0, width, height};
    add_camera_metadata_entry(meta, ANDROID_SCALER_CROP_REGION, cropRegion, 4);

    size_t metaSize = get_camera_metadata_size(meta);
    std::vector<uint8_t> result(metaSize);
    memcpy(result.data(), meta, metaSize);
    free_camera_metadata(meta);

    return result;
}

}  // namespace virtualcamera
