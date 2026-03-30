/*
 * Virtual Display HWC Service entry point
 */

#define LOG_TAG "VirtualDisplayHWC"

#include "VirtualDisplayComposer.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::android::hardware::graphics::composer3::VirtualDisplayComposer;

int main() {
    LOG(INFO) << "Virtual Display HWC service starting...";
    
    // HWC needs multiple threads for callbacks
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    
    auto composer = ndk::SharedRefBase::make<VirtualDisplayComposer>();
    
    const std::string instanceName = 
        std::string() + VirtualDisplayComposer::descriptor + "/default";
    
    binder_status_t status = AServiceManager_addService(
        composer->asBinder().get(), instanceName.c_str());
    
    if (status != STATUS_OK) {
        LOG(ERROR) << "Failed to register HWC service: " << status;
        return 1;
    }
    
    LOG(INFO) << "Virtual Display HWC service registered: " << instanceName;
    
    ABinderProcess_joinThreadPool();
    
    return 0;  // Should never reach here
}
