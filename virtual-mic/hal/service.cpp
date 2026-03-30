/*
 * Virtual Microphone HAL Service entry point
 */

#define LOG_TAG "android.hardware.audio-virtual-mic-service"

#include "VirtualMicModule.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::android::hardware::audio::core::VirtualMicModule;

int main() {
    LOG(INFO) << "Virtual Microphone HAL service starting...";
    
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    
    auto module = ndk::SharedRefBase::make<VirtualMicModule>();
    
    const std::string instanceName = 
        std::string() + VirtualMicModule::descriptor + "/virtual_mic";
    
    binder_status_t status = AServiceManager_addService(
        module->asBinder().get(), instanceName.c_str());
    
    if (status != STATUS_OK) {
        LOG(ERROR) << "Failed to register service: " << status;
        return 1;
    }
    
    LOG(INFO) << "Virtual Microphone HAL service registered: " << instanceName;
    
    ABinderProcess_joinThreadPool();
    
    return 0;  // Should never reach here
}
