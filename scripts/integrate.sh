#!/bin/bash
#
# integrate.sh - Integrate Virtual Camera into AOSP tree
#
# Usage: ./integrate.sh /path/to/aosp
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCAM_ROOT="$(dirname "$SCRIPT_DIR")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Check arguments
AOSP_ROOT="${1:-/mnt/micron/aosp}"

if [ ! -d "$AOSP_ROOT/build/make" ]; then
    error "AOSP root not found at $AOSP_ROOT"
fi

info "Integrating Virtual Camera into $AOSP_ROOT"

# 1. Create directories
info "Creating directories..."
mkdir -p "$AOSP_ROOT/frameworks/base/core/java/android/hardware/camera/virtual"
mkdir -p "$AOSP_ROOT/frameworks/base/services/core/java/com/android/server/camera/virtual"
mkdir -p "$AOSP_ROOT/hardware/interfaces/camera/provider/virtual"
mkdir -p "$AOSP_ROOT/device/google/cuttlefish/sepolicy/vendor/virtual_camera"

# 2. Copy AIDL interfaces
info "Copying AIDL interfaces..."
cp "$VCAM_ROOT/aidl/"*.aidl \
   "$AOSP_ROOT/frameworks/base/core/java/android/hardware/camera/virtual/"

# 3. Copy system service
info "Copying system service..."
cp "$VCAM_ROOT/service/VirtualCameraService.java" \
   "$VCAM_ROOT/service/VirtualCamera.java" \
   "$AOSP_ROOT/frameworks/base/services/core/java/com/android/server/camera/virtual/"

# 4. Copy HAL
info "Copying Camera HAL..."
cp "$VCAM_ROOT/hal/"* \
   "$AOSP_ROOT/hardware/interfaces/camera/provider/virtual/"

# 5. Copy SELinux policies
info "Copying SELinux policies..."
cp "$VCAM_ROOT/sepolicy/"*.te \
   "$VCAM_ROOT/sepolicy/"*_contexts \
   "$AOSP_ROOT/device/google/cuttlefish/sepolicy/vendor/virtual_camera/" 2>/dev/null || true

# 6. Create Android.bp for AIDL
info "Creating AIDL build file..."
cat > "$AOSP_ROOT/frameworks/base/core/java/android/hardware/camera/virtual/Android.bp" << 'EOF'
filegroup {
    name: "virtual-camera-aidl",
    srcs: [
        "IVirtualCameraService.aidl",
        "IVirtualCameraCallback.aidl",
        "IVirtualCameraManager.aidl",
        "VirtualCameraConfig.aidl",
        "StreamConfig.aidl",
    ],
    path: ".",
}
EOF

# 7. Create service init.rc
info "Creating HAL init.rc..."
cat > "$AOSP_ROOT/hardware/interfaces/camera/provider/virtual/android.hardware.camera.provider-virtual-service.rc" << 'EOF'
service vendor.camera.provider-virtual /vendor/bin/hw/android.hardware.camera.provider-virtual-service
    class hal
    user cameraserver
    group audio camera input drmrpc
    ioprio rt 4
    capabilities SYS_NICE
    task_profiles CameraServiceCapacity MaxPerformance
EOF

# 8. Create VINTF manifest fragment
info "Creating VINTF manifest..."
cat > "$AOSP_ROOT/hardware/interfaces/camera/provider/virtual/android.hardware.camera.provider-virtual-service.xml" << 'EOF'
<manifest version="1.0" type="device">
    <hal format="aidl">
        <name>android.hardware.camera.provider</name>
        <version>2</version>
        <fqname>ICameraProvider/virtual/0</fqname>
    </hal>
</manifest>
EOF

# 9. Create service.cpp
info "Creating HAL service main..."
cat > "$AOSP_ROOT/hardware/interfaces/camera/provider/virtual/service.cpp" << 'EOF'
#define LOG_TAG "android.hardware.camera.provider-virtual-service"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include "VirtualCameraProvider.h"

using aidl::android::hardware::camera::provider::implementation::VirtualCameraProvider;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    
    auto provider = ndk::SharedRefBase::make<VirtualCameraProvider>();
    const std::string name = std::string() + VirtualCameraProvider::descriptor + "/virtual/0";
    
    auto status = AServiceManager_addService(provider->asBinder().get(), name.c_str());
    CHECK_EQ(status, STATUS_OK) << "Failed to register camera provider";
    
    LOG(INFO) << "Virtual Camera Provider registered";
    
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
EOF

info "Integration complete!"
echo ""
echo "Next steps:"
echo "  1. Patch SystemServer.java (see service/SystemServer.patch)"
echo "  2. Add to device makefile:"
echo "     PRODUCT_PACKAGES += android.hardware.camera.provider-virtual-service"
echo "  3. Build: m -j\$(nproc)"
echo ""
