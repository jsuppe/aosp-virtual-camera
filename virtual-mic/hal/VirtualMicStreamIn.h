/*
 * VirtualMicStreamIn - Input stream implementation for virtual microphone
 */
#pragma once

#include <aidl/android/hardware/audio/core/BnStreamIn.h>
#include <aidl/android/hardware/audio/core/StreamDescriptor.h>

#include <atomic>
#include <mutex>

namespace aidl::android::hardware::audio::core {

class VirtualMicModule;

class VirtualMicStreamIn : public BnStreamIn {
public:
    explicit VirtualMicStreamIn(VirtualMicModule* module);
    ~VirtualMicStreamIn() override;

    // IStreamCommon interface
    ndk::ScopedAStatus close() override;
    ndk::ScopedAStatus prepareToClose() override;
    ndk::ScopedAStatus updateHwAvSyncId(int32_t in_hwAvSyncId) override;
    ndk::ScopedAStatus getVendorParameters(
            const std::vector<std::string>& in_ids,
            std::vector<VendorParameter>* _aidl_return) override;
    ndk::ScopedAStatus setVendorParameters(
            const std::vector<VendorParameter>& in_parameters, bool in_async) override;
    ndk::ScopedAStatus addEffect(
            const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) override;
    ndk::ScopedAStatus removeEffect(
            const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) override;

    // IStreamIn interface  
    ndk::ScopedAStatus getStreamCommon(std::shared_ptr<IStreamCommon>* _aidl_return) override;
    ndk::ScopedAStatus getActiveMicrophones(
            std::vector<MicrophoneDynamicInfo>* _aidl_return) override;
    ndk::ScopedAStatus getMicrophoneDirection(MicrophoneDirection* _aidl_return) override;
    ndk::ScopedAStatus setMicrophoneDirection(MicrophoneDirection in_direction) override;
    ndk::ScopedAStatus getMicrophoneFieldDimension(float* _aidl_return) override;
    ndk::ScopedAStatus setMicrophoneFieldDimension(float in_zoom) override;
    ndk::ScopedAStatus getHwGain(std::vector<float>* _aidl_return) override;
    ndk::ScopedAStatus setHwGain(const std::vector<float>& in_channelGains) override;

    // Read audio data (called by framework via FMQ)
    size_t readAudioData(void* buffer, size_t sizeBytes);

private:
    VirtualMicModule* mModule;  // Non-owning pointer
    std::atomic<bool> mClosed{false};
    MicrophoneDirection mMicDirection = MicrophoneDirection::FRONT;
    float mMicFieldDimension = 0.0f;
    std::vector<float> mHwGain{1.0f, 1.0f};
};

}  // namespace aidl::android::hardware::audio::core
