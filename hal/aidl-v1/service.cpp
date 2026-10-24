/*
 * Virtual Camera Provider Service - AIDL V1
 */

#define LOG_TAG "VirtualCameraService"

#include "VirtualCameraProvider.h"
#include "VirtualCameraStableHal.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::android::hardware::camera::provider::implementation::VirtualCameraProvider;
#ifdef VCAM_STABLE_AIDL
using aidl::android::hardware::camera::provider::implementation::VirtualCameraStableHal;
#endif

int main() {
    LOG(INFO) << "Virtual Camera Provider starting (AIDL V1)...";

    // A few binder threads: capture requests and platform frame pushes
    // (queueFrame) arrive concurrently.
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();

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

#ifdef VCAM_STABLE_AIDL
    if (!VirtualCameraStableHal::init(provider.get())) {
        LOG(ERROR) << "Failed to register IVirtualCameraHal";
        // Non-fatal: camera stays hidden (no producer availability signal).
    }
#endif
    LOG(INFO) << "Virtual Camera Provider service started successfully (AIDL V1)";
    ABinderProcess_joinThreadPool();

    return 0;
}
