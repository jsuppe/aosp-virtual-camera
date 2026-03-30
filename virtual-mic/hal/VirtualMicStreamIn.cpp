/*
 * VirtualMicStreamIn - Input stream implementation for virtual microphone
 */

#define LOG_TAG "VirtualMicStreamIn"

#include "VirtualMicStreamIn.h"
#include "VirtualMicModule.h"

#include <android-base/logging.h>

namespace aidl::android::hardware::audio::core {

VirtualMicStreamIn::VirtualMicStreamIn(VirtualMicModule* module)
    : mModule(module) {
    LOG(INFO) << "VirtualMicStreamIn created";
}

VirtualMicStreamIn::~VirtualMicStreamIn() {
    LOG(INFO) << "VirtualMicStreamIn destroyed";
}

// IStreamCommon interface

ndk::ScopedAStatus VirtualMicStreamIn::close() {
    LOG(INFO) << "close";
    mClosed = true;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicStreamIn::prepareToClose() {
    LOG(DEBUG) << "prepareToClose";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicStreamIn::updateHwAvSyncId(int32_t in_hwAvSyncId) {
    LOG(DEBUG) << "updateHwAvSyncId: " << in_hwAvSyncId;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicStreamIn::getVendorParameters(
        const std::vector<std::string>& in_ids,
        std::vector<VendorParameter>* _aidl_return) {
    _aidl_return->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicStreamIn::setVendorParameters(
        const std::vector<VendorParameter>& in_parameters, bool in_async) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicStreamIn::addEffect(
        const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) {
    // Effects not supported on virtual mic
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus VirtualMicStreamIn::removeEffect(
        const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

// IStreamIn interface

ndk::ScopedAStatus VirtualMicStreamIn::getStreamCommon(
        std::shared_ptr<IStreamCommon>* _aidl_return) {
    *_aidl_return = this->ref<IStreamCommon>();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicStreamIn::getActiveMicrophones(
        std::vector<MicrophoneDynamicInfo>* _aidl_return) {
    MicrophoneDynamicInfo info;
    info.id = "virtual_mic_0";
    info.channelMapping = {
        MicrophoneDynamicInfo::ChannelMapping::DIRECT,
        MicrophoneDynamicInfo::ChannelMapping::DIRECT
    };
    *_aidl_return = {info};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicStreamIn::getMicrophoneDirection(
        MicrophoneDirection* _aidl_return) {
    *_aidl_return = mMicDirection;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicStreamIn::setMicrophoneDirection(
        MicrophoneDirection in_direction) {
    mMicDirection = in_direction;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicStreamIn::getMicrophoneFieldDimension(float* _aidl_return) {
    *_aidl_return = mMicFieldDimension;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicStreamIn::setMicrophoneFieldDimension(float in_zoom) {
    mMicFieldDimension = in_zoom;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicStreamIn::getHwGain(std::vector<float>* _aidl_return) {
    *_aidl_return = mHwGain;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicStreamIn::setHwGain(const std::vector<float>& in_channelGains) {
    mHwGain = in_channelGains;
    return ndk::ScopedAStatus::ok();
}

// Read audio data
size_t VirtualMicStreamIn::readAudioData(void* buffer, size_t sizeBytes) {
    if (mClosed) {
        return 0;
    }
    
    // TODO: Read from module's ring buffer
    // For now, generate silence (zeros)
    memset(buffer, 0, sizeBytes);
    return sizeBytes;
}

}  // namespace aidl::android::hardware::audio::core
