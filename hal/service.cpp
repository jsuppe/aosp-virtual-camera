/*
 * Virtual Camera Provider Service
 */

#define LOG_TAG "VirtualCameraService"

#include "VirtualCameraProvider.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::android::hardware::camera::provider::implementation::VirtualCameraProvider;

int main() {
    LOG(INFO) << "Virtual Camera Provider starting...";
    
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    
    auto provider = ndk::SharedRefBase::make<VirtualCameraProvider>();
    const std::string instance = 
            std::string(VirtualCameraProvider::descriptor) + "/virtual_renderer/0";
    
    LOG(INFO) << "Registering service: " << instance;
    
    binder_status_t status = AServiceManager_addService(
            provider->asBinder().get(), instance.c_str());
    
    if (status != STATUS_OK) {
        LOG(ERROR) << "Failed to register service: " << status;
        return -1;
    }
    
    LOG(INFO) << "Virtual Camera Provider service started successfully";
    ABinderProcess_joinThreadPool();
    
    return 0;
}
