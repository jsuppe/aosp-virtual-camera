/*
 * VirtualMicModule - AIDL Audio HAL IModule implementation
 */

#define LOG_TAG "VirtualMicModule"

#include "VirtualMicModule.h"
#include "VirtualMicStreamIn.h"

#include <android-base/logging.h>
#include <aidl/android/media/audio/common/AudioDeviceAddress.h>
#include <aidl/android/media/audio/common/AudioDeviceDescription.h>
#include <aidl/android/media/audio/common/AudioFormatDescription.h>
#include <aidl/android/media/audio/common/AudioIoFlags.h>
#include <aidl/android/media/audio/common/AudioOutputFlags.h>
#include <aidl/android/media/audio/common/AudioInputFlags.h>
#include <aidl/android/media/audio/common/Int.h>
#include <aidl/android/media/audio/common/PcmType.h>

namespace aidl::android::hardware::audio::core {

using ::aidl::android::media::audio::common::AudioChannelLayout;
using ::aidl::android::media::audio::common::AudioDeviceAddress;
using ::aidl::android::media::audio::common::AudioDeviceDescription;
using ::aidl::android::media::audio::common::AudioFormatDescription;
using ::aidl::android::media::audio::common::AudioFormatType;
using ::aidl::android::media::audio::common::AudioIoFlags;
using ::aidl::android::media::audio::common::AudioInputFlags;
using ::aidl::android::media::audio::common::AudioPortDeviceExt;
using ::aidl::android::media::audio::common::AudioPortExt;
using ::aidl::android::media::audio::common::AudioPortMixExt;
using ::aidl::android::media::audio::common::Int;
using ::aidl::android::media::audio::common::PcmType;

VirtualMicModule::VirtualMicModule() {
    LOG(INFO) << "VirtualMicModule created";
    mRingBuffer.resize(kRingBufferSizeBytes);
    initializePorts();
}

void VirtualMicModule::initializePorts() {
    std::lock_guard<std::mutex> lock(mLock);
    
    // Create the virtual mic device port
    AudioPort micPort = createVirtualMicPort();
    mPorts[micPort.id] = micPort;
    
    // Create a mix port for the input stream
    AudioPort mixPort;
    mixPort.id = mNextPortId++;
    mixPort.name = "virtual_mic_input";
    mixPort.flags = AudioIoFlags::make<AudioIoFlags::Tag::input>(
        AudioInputFlags::PRIMARY | AudioInputFlags::HW_AV_SYNC);
    
    // Supported format: 48kHz stereo 16-bit
    AudioProfile profile;
    profile.format.type = AudioFormatType::PCM;
    profile.format.pcm = PcmType::INT_16_BIT;
    profile.sampleRates = {48000, 44100, 16000};
    profile.channelMasks = {
        AudioChannelLayout::make<AudioChannelLayout::Tag::layoutMask>(
            AudioChannelLayout::LAYOUT_MONO),
        AudioChannelLayout::make<AudioChannelLayout::Tag::layoutMask>(
            AudioChannelLayout::LAYOUT_STEREO)
    };
    mixPort.profiles = {profile};
    
    AudioPortMixExt mixExt;
    mixExt.maxOpenStreamCount = 1;
    mixPort.ext = AudioPortExt::make<AudioPortExt::Tag::mix>(mixExt);
    
    mPorts[mixPort.id] = mixPort;
    
    LOG(INFO) << "Initialized " << mPorts.size() << " ports";
}

AudioPort VirtualMicModule::createVirtualMicPort() {
    AudioPort port;
    port.id = mNextPortId++;
    port.name = "Virtual Microphone";
    port.flags = AudioIoFlags::make<AudioIoFlags::Tag::input>(0);
    
    // Audio profile: 48kHz stereo 16-bit PCM
    AudioProfile profile;
    profile.format.type = AudioFormatType::PCM;
    profile.format.pcm = PcmType::INT_16_BIT;
    profile.sampleRates = {48000, 44100, 16000};
    profile.channelMasks = {
        AudioChannelLayout::make<AudioChannelLayout::Tag::layoutMask>(
            AudioChannelLayout::LAYOUT_MONO),
        AudioChannelLayout::make<AudioChannelLayout::Tag::layoutMask>(
            AudioChannelLayout::LAYOUT_STEREO)
    };
    port.profiles = {profile};
    
    // Device extension - appears as USB device (configurable later)
    AudioPortDeviceExt deviceExt;
    deviceExt.device.type.type = AudioDeviceType::IN_MICROPHONE;
    deviceExt.device.type.connection = "";
    deviceExt.device.address = AudioDeviceAddress::make<AudioDeviceAddress::Tag::id>(
        "virtual_mic_0");
    port.ext = AudioPortExt::make<AudioPortExt::Tag::device>(deviceExt);
    
    return port;
}

// --- IModule interface implementation ---

ndk::ScopedAStatus VirtualMicModule::setModuleDebug(const ModuleDebug& in_debug) {
    LOG(DEBUG) << "setModuleDebug";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getTelephony(std::shared_ptr<ITelephony>* _aidl_return) {
    *_aidl_return = nullptr;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getBluetooth(std::shared_ptr<IBluetooth>* _aidl_return) {
    *_aidl_return = nullptr;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getBluetoothA2dp(std::shared_ptr<IBluetoothA2dp>* _aidl_return) {
    *_aidl_return = nullptr;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getBluetoothLe(std::shared_ptr<IBluetoothLe>* _aidl_return) {
    *_aidl_return = nullptr;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::connectExternalDevice(
        const AudioPort& in_templateIdAndAdditionalData, AudioPort* _aidl_return) {
    // Not supporting dynamic device connection for now
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus VirtualMicModule::disconnectExternalDevice(int32_t in_portId) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus VirtualMicModule::getAudioPatches(std::vector<AudioPatch>* _aidl_return) {
    std::lock_guard<std::mutex> lock(mLock);
    _aidl_return->clear();
    for (const auto& [id, patch] : mPatches) {
        _aidl_return->push_back(patch);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getAudioPort(int32_t in_portId, AudioPort* _aidl_return) {
    std::lock_guard<std::mutex> lock(mLock);
    auto it = mPorts.find(in_portId);
    if (it == mPorts.end()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    *_aidl_return = it->second;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getAudioPortConfigs(std::vector<AudioPortConfig>* _aidl_return) {
    std::lock_guard<std::mutex> lock(mLock);
    _aidl_return->clear();
    for (const auto& [id, config] : mPortConfigs) {
        _aidl_return->push_back(config);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getAudioPorts(std::vector<AudioPort>* _aidl_return) {
    std::lock_guard<std::mutex> lock(mLock);
    _aidl_return->clear();
    for (const auto& [id, port] : mPorts) {
        _aidl_return->push_back(port);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getAudioRoutes(std::vector<AudioRoute>* _aidl_return) {
    std::lock_guard<std::mutex> lock(mLock);
    // Single route: virtual mic device -> mix port
    AudioRoute route;
    route.sinkPortId = 2;  // mix port
    route.sourcePortIds = {1};  // device port
    route.isExclusive = true;
    *_aidl_return = {route};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getAudioRoutesForAudioPort(
        int32_t in_portId, std::vector<AudioRoute>* _aidl_return) {
    return getAudioRoutes(_aidl_return);
}

ndk::ScopedAStatus VirtualMicModule::openInputStream(
        const OpenInputStreamArguments& in_args, OpenInputStreamReturn* _aidl_return) {
    LOG(INFO) << "openInputStream: portConfigId=" << in_args.portConfigId;
    
    // Create the input stream
    auto stream = ndk::SharedRefBase::make<VirtualMicStreamIn>(this);
    
    mStreamActive = true;
    mActiveInputStream = stream;
    
    // Return stream descriptor
    _aidl_return->stream = stream;
    _aidl_return->desc.portId = in_args.portConfigId;
    _aidl_return->desc.bufferSizeFrames = 960;  // 20ms at 48kHz
    // TODO: Set up FMQ descriptors for audio data transfer
    
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::openOutputStream(
        const OpenOutputStreamArguments& in_args, OpenOutputStreamReturn* _aidl_return) {
    // Virtual mic doesn't support output
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus VirtualMicModule::getSupportedPlaybackRateFactors(
        SupportedPlaybackRateFactors* _aidl_return) {
    _aidl_return->minSpeed = 1.0f;
    _aidl_return->maxSpeed = 1.0f;
    _aidl_return->minPitch = 1.0f;
    _aidl_return->maxPitch = 1.0f;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::setAudioPatch(
        const AudioPatch& in_requested, AudioPatch* _aidl_return) {
    std::lock_guard<std::mutex> lock(mLock);
    AudioPatch patch = in_requested;
    patch.id = mNextPatchId++;
    mPatches[patch.id] = patch;
    *_aidl_return = patch;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::setAudioPortConfig(
        const AudioPortConfig& in_requested, AudioPortConfig* _aidl_return, bool* _aidl_retval) {
    std::lock_guard<std::mutex> lock(mLock);
    AudioPortConfig config = in_requested;
    if (config.id == 0) {
        config.id = mNextPortConfigId++;
    }
    mPortConfigs[config.id] = config;
    *_aidl_return = config;
    *_aidl_retval = true;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::resetAudioPatch(int32_t in_patchId) {
    std::lock_guard<std::mutex> lock(mLock);
    mPatches.erase(in_patchId);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::resetAudioPortConfig(int32_t in_portConfigId) {
    std::lock_guard<std::mutex> lock(mLock);
    mPortConfigs.erase(in_portConfigId);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getMasterMute(bool* _aidl_return) {
    *_aidl_return = false;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::setMasterMute(bool in_mute) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getMasterVolume(float* _aidl_return) {
    *_aidl_return = 1.0f;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::setMasterVolume(float in_volume) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getMicMute(bool* _aidl_return) {
    *_aidl_return = mMicMute.load();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::setMicMute(bool in_mute) {
    mMicMute = in_mute;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getMicrophones(std::vector<MicrophoneInfo>* _aidl_return) {
    MicrophoneInfo mic;
    mic.id = "virtual_mic_0";
    mic.device.type.type = AudioDeviceType::IN_MICROPHONE;
    mic.device.address = AudioDeviceAddress::make<AudioDeviceAddress::Tag::id>("virtual_mic_0");
    mic.location = MicrophoneInfo::Location::UNKNOWN;
    mic.group = 0;
    mic.indexInTheGroup = 0;
    mic.sensitivity = -37.0f;  // dB
    mic.maxSpl = 132.5f;  // dB
    mic.minSpl = 28.5f;   // dB
    mic.directionality = MicrophoneInfo::Directionality::OMNI;
    *_aidl_return = {mic};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::updateAudioMode(AudioMode in_mode) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::updateScreenRotation(ScreenRotation in_rotation) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::updateScreenState(bool in_isTurnedOn) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getSoundDose(std::shared_ptr<ISoundDose>* _aidl_return) {
    *_aidl_return = nullptr;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::generateHwAvSyncId(int32_t* _aidl_return) {
    static int32_t sNextHwAvSyncId = 1;
    *_aidl_return = sNextHwAvSyncId++;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getVendorParameters(
        const std::vector<std::string>& in_ids, std::vector<VendorParameter>* _aidl_return) {
    _aidl_return->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::setVendorParameters(
        const std::vector<VendorParameter>& in_parameters, bool in_async) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::addDeviceEffect(
        int32_t in_portConfigId,
        const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus VirtualMicModule::removeDeviceEffect(
        int32_t in_portConfigId,
        const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus VirtualMicModule::getMmapPolicyInfos(
        AudioMMapPolicyType in_policyType, std::vector<AudioMMapPolicyInfo>* _aidl_return) {
    _aidl_return->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::supportsVariableLatency(bool* _aidl_return) {
    *_aidl_return = false;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getAAudioMixerBurstCount(int32_t* _aidl_return) {
    *_aidl_return = 2;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::getAAudioHardwareBurstMinUsec(int32_t* _aidl_return) {
    *_aidl_return = 2000;  // 2ms
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualMicModule::prepareToDisconnectExternalDevice(int32_t in_portId) {
    return ndk::ScopedAStatus::ok();
}

// --- Virtual Mic specific methods ---

bool VirtualMicModule::writeAudioData(const void* buffer, size_t sizeBytes) {
    if (!mStreamActive) {
        return false;
    }
    
    // Write to ring buffer
    const uint8_t* src = static_cast<const uint8_t*>(buffer);
    size_t writePos = mRingBufferWritePos.load();
    
    for (size_t i = 0; i < sizeBytes; ++i) {
        mRingBuffer[(writePos + i) % kRingBufferSizeBytes] = src[i];
    }
    
    mRingBufferWritePos = (writePos + sizeBytes) % kRingBufferSizeBytes;
    return true;
}

}  // namespace aidl::android::hardware::audio::core
