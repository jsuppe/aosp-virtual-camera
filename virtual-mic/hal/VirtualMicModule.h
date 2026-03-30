/*
 * VirtualMicModule - AIDL Audio HAL IModule implementation
 * 
 * Implements a virtual microphone that receives audio from renderer apps
 * and provides it to consumer apps as microphone input.
 */
#pragma once

#include <aidl/android/hardware/audio/core/BnModule.h>
#include <aidl/android/hardware/audio/core/IStreamIn.h>
#include <aidl/android/hardware/audio/core/StreamDescriptor.h>
#include <aidl/android/media/audio/common/AudioPort.h>
#include <aidl/android/media/audio/common/AudioPortConfig.h>

#include <mutex>
#include <memory>
#include <unordered_map>

namespace aidl::android::hardware::audio::core {

using ::aidl::android::media::audio::common::AudioPort;
using ::aidl::android::media::audio::common::AudioPortConfig;
using ::aidl::android::media::audio::common::AudioDeviceType;
using ::aidl::android::media::audio::common::AudioProfile;

class VirtualMicModule : public BnModule {
public:
    VirtualMicModule();
    ~VirtualMicModule() override = default;

    // IModule interface
    ndk::ScopedAStatus setModuleDebug(const ModuleDebug& in_debug) override;
    
    ndk::ScopedAStatus getTelephony(std::shared_ptr<ITelephony>* _aidl_return) override;
    ndk::ScopedAStatus getBluetooth(std::shared_ptr<IBluetooth>* _aidl_return) override;
    ndk::ScopedAStatus getBluetoothA2dp(std::shared_ptr<IBluetoothA2dp>* _aidl_return) override;
    ndk::ScopedAStatus getBluetoothLe(std::shared_ptr<IBluetoothLe>* _aidl_return) override;
    
    ndk::ScopedAStatus connectExternalDevice(const AudioPort& in_templateIdAndAdditionalData,
                                             AudioPort* _aidl_return) override;
    ndk::ScopedAStatus disconnectExternalDevice(int32_t in_portId) override;
    
    ndk::ScopedAStatus getAudioPatches(std::vector<AudioPatch>* _aidl_return) override;
    ndk::ScopedAStatus getAudioPort(int32_t in_portId, AudioPort* _aidl_return) override;
    ndk::ScopedAStatus getAudioPortConfigs(std::vector<AudioPortConfig>* _aidl_return) override;
    ndk::ScopedAStatus getAudioPorts(std::vector<AudioPort>* _aidl_return) override;
    ndk::ScopedAStatus getAudioRoutes(std::vector<AudioRoute>* _aidl_return) override;
    ndk::ScopedAStatus getAudioRoutesForAudioPort(int32_t in_portId, 
                                                   std::vector<AudioRoute>* _aidl_return) override;
    
    ndk::ScopedAStatus openInputStream(const OpenInputStreamArguments& in_args,
                                       OpenInputStreamReturn* _aidl_return) override;
    ndk::ScopedAStatus openOutputStream(const OpenOutputStreamArguments& in_args,
                                        OpenOutputStreamReturn* _aidl_return) override;
    
    ndk::ScopedAStatus getSupportedPlaybackRateFactors(
            SupportedPlaybackRateFactors* _aidl_return) override;
    
    ndk::ScopedAStatus setAudioPatch(const AudioPatch& in_requested,
                                     AudioPatch* _aidl_return) override;
    ndk::ScopedAStatus setAudioPortConfig(const AudioPortConfig& in_requested,
                                          AudioPortConfig* _aidl_return, 
                                          bool* _aidl_retval) override;
    ndk::ScopedAStatus resetAudioPatch(int32_t in_patchId) override;
    ndk::ScopedAStatus resetAudioPortConfig(int32_t in_portConfigId) override;
    
    ndk::ScopedAStatus getMasterMute(bool* _aidl_return) override;
    ndk::ScopedAStatus setMasterMute(bool in_mute) override;
    ndk::ScopedAStatus getMasterVolume(float* _aidl_return) override;
    ndk::ScopedAStatus setMasterVolume(float in_volume) override;
    
    ndk::ScopedAStatus getMicMute(bool* _aidl_return) override;
    ndk::ScopedAStatus setMicMute(bool in_mute) override;
    
    ndk::ScopedAStatus getMicrophones(std::vector<MicrophoneInfo>* _aidl_return) override;
    
    ndk::ScopedAStatus updateAudioMode(AudioMode in_mode) override;
    ndk::ScopedAStatus updateScreenRotation(ScreenRotation in_rotation) override;
    ndk::ScopedAStatus updateScreenState(bool in_isTurnedOn) override;
    
    ndk::ScopedAStatus getSoundDose(std::shared_ptr<ISoundDose>* _aidl_return) override;
    ndk::ScopedAStatus generateHwAvSyncId(int32_t* _aidl_return) override;
    
    ndk::ScopedAStatus getVendorParameters(const std::vector<std::string>& in_ids,
                                           std::vector<VendorParameter>* _aidl_return) override;
    ndk::ScopedAStatus setVendorParameters(const std::vector<VendorParameter>& in_parameters,
                                           bool in_async) override;
    
    ndk::ScopedAStatus addDeviceEffect(int32_t in_portConfigId,
                                       const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) override;
    ndk::ScopedAStatus removeDeviceEffect(int32_t in_portConfigId,
                                          const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) override;
    
    ndk::ScopedAStatus getMmapPolicyInfos(AudioMMapPolicyType in_policyType,
                                          std::vector<AudioMMapPolicyInfo>* _aidl_return) override;
    ndk::ScopedAStatus supportsVariableLatency(bool* _aidl_return) override;
    ndk::ScopedAStatus getAAudioMixerBurstCount(int32_t* _aidl_return) override;
    ndk::ScopedAStatus getAAudioHardwareBurstMinUsec(int32_t* _aidl_return) override;
    
    ndk::ScopedAStatus prepareToDisconnectExternalDevice(int32_t in_portId) override;

    // --- Virtual Mic specific methods ---
    
    // Write audio data from renderer (called by VirtualMicService)
    bool writeAudioData(const void* buffer, size_t sizeBytes);
    
    // Get current stream configuration
    bool isStreamActive() const { return mStreamActive; }

private:
    // Initialize device ports and capabilities
    void initializePorts();
    
    // Create AudioPort for the virtual mic device
    AudioPort createVirtualMicPort();
    
    std::mutex mLock;
    
    // Device configuration
    int32_t mNextPortId = 1;
    int32_t mNextPortConfigId = 1;
    int32_t mNextPatchId = 1;
    
    std::unordered_map<int32_t, AudioPort> mPorts;
    std::unordered_map<int32_t, AudioPortConfig> mPortConfigs;
    std::unordered_map<int32_t, AudioPatch> mPatches;
    
    // Stream state
    std::atomic<bool> mStreamActive{false};
    std::atomic<bool> mMicMute{false};
    
    // Active input stream (weak reference to avoid circular dependency)
    std::weak_ptr<IStreamIn> mActiveInputStream;
    
    // Ring buffer for audio data from renderer
    static constexpr size_t kRingBufferSizeBytes = 48000 * 2 * 2 * 1;  // 1 second @ 48kHz stereo 16-bit
    std::vector<uint8_t> mRingBuffer;
    std::atomic<size_t> mRingBufferWritePos{0};
    std::atomic<size_t> mRingBufferReadPos{0};
};

}  // namespace aidl::android::hardware::audio::core
