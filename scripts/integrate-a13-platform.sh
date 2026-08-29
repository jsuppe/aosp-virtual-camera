#!/bin/bash
#
# integrate-a13-platform.sh — Integrate the platform-AIDL virtual camera into
# an Android 13 AOSP tree (Cuttlefish).
#
# Idempotent: safe to re-run after repo changes.
#
# What it does:
#  1. HAL sources (core + v1 adapter) -> hardware/interfaces/camera/provider/virtual/
#     with the A13 PLATFORM Android.bp variants (system_ext + AIDL relay mode)
#  2. Platform AIDL lib -> .../virtual/platform-aidl/
#  3. VirtualCameraService java lib -> .../virtual/platform-service/
#  4. Test apps -> .../virtual/apps/{VCamProducer,VCamViewer}
#  5. frameworks/base: services static_libs += virtual-camera-service,
#     SystemServer registers VirtualCameraService.Lifecycle
#  6. sepolicy -> device/google/cuttlefish/shared/sepolicy/system_ext/private/
#  7. PRODUCT_PACKAGES += apps (device/google/cuttlefish/shared/device.mk)
#
# Usage: ./integrate-a13-platform.sh /mnt/micron/aosp-a13

set -e

AOSP_ROOT="${1:?Usage: $0 <aosp_root>}"
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"   # repo root
HAL_DEST="$AOSP_ROOT/hardware/interfaces/camera/provider/virtual"
SEPOLICY_DEST="$AOSP_ROOT/device/google/cuttlefish/shared/sepolicy/system_ext/private"
DEVICE_MK="$AOSP_ROOT/device/google/cuttlefish/shared/device.mk"

[ -f "$AOSP_ROOT/build/envsetup.sh" ] || { echo "not an AOSP tree: $AOSP_ROOT"; exit 1; }

echo "=== [0/7] Stable AIDL boundary + platform JNI + APEX (vendor packaging) ==="
rm -rf "$HAL_DEST/stable-aidl" "$HAL_DEST/platform-jni" "$HAL_DEST/apex"
cp -r "$SCRIPT_DIR/stable-aidl"   "$HAL_DEST/stable-aidl"
cp -r "$SCRIPT_DIR/platform-jni"  "$HAL_DEST/platform-jni"
cp -r "$SCRIPT_DIR/apex"          "$HAL_DEST/apex"

echo "=== [1/7] HAL sources (A13 platform variant) ==="
mkdir -p "$HAL_DEST/core" "$HAL_DEST/aidl"
cp "$SCRIPT_DIR"/hal/core/*.cpp "$SCRIPT_DIR"/hal/core/*.h "$HAL_DEST/core/"
cp "$SCRIPT_DIR"/hal/aidl-v1/*.cpp "$SCRIPT_DIR"/hal/aidl-v1/*.h "$HAL_DEST/aidl/"
cp "$SCRIPT_DIR/platform/bp/core.Android.bp"      "$HAL_DEST/core/Android.bp"
cp "$SCRIPT_DIR/platform/bp/aidl-v1.Android.bp"   "$HAL_DEST/aidl/Android.bp"
cp "$SCRIPT_DIR/platform/rc/android.hardware.camera.provider-virtual-service.rc"   "$HAL_DEST/aidl/"
cp "$SCRIPT_DIR/platform/vintf/android.hardware.camera.provider-virtual-service.xml" "$HAL_DEST/aidl/"
# top-level bp: nothing to build directly
cat > "$HAL_DEST/Android.bp" << "EOF"
// Virtual camera HAL — see core/ and aidl/ (A13 platform build)
EOF

echo "=== [2/7] Platform AIDL lib ==="
rm -rf "$HAL_DEST/platform-aidl"
cp -r "$SCRIPT_DIR/platform/aidl-lib" "$HAL_DEST/platform-aidl"
AIDL_DEST="$HAL_DEST/platform-aidl/android/hardware/virtualcamera"
mkdir -p "$AIDL_DEST"
cp "$SCRIPT_DIR"/aidl/*.aidl "$AIDL_DEST/"

echo "=== [3/7] VirtualCameraService java lib ==="
SVC_DEST="$HAL_DEST/platform-service"
mkdir -p "$SVC_DEST/java/com/android/server/camera/virtual"
cp "$SCRIPT_DIR"/service/VirtualCameraService.java "$SCRIPT_DIR"/service/VirtualCamera.java \
   "$SCRIPT_DIR"/service/VirtualCameraNative.java \
   "$SVC_DEST/java/com/android/server/camera/virtual/"
cp "$SCRIPT_DIR/platform/services/Android.bp" "$SVC_DEST/Android.bp"

echo "=== [4/7] Test apps ==="
rm -rf "$HAL_DEST/apps"
mkdir -p "$HAL_DEST/apps"
cp -r "$SCRIPT_DIR/platform/apps/VCamProducer" "$HAL_DEST/apps/"
cp -r "$SCRIPT_DIR/platform/apps/VCamViewer" "$HAL_DEST/apps/"

echo "=== [5/7] frameworks/base hooks ==="
python3 - "$AOSP_ROOT" << "PYEOF"
import sys, pathlib
root = pathlib.Path(sys.argv[1])

# 5a. services/Android.bp: static_libs += virtual-camera-service
bp = root / "frameworks/base/services/Android.bp"
t = bp.read_text()
if "virtual-camera-service" not in t:
    anchor = "\"services.core\","
    assert anchor in t, "services.core anchor not found in services/Android.bp"
    t = t.replace(anchor, anchor + "\n        \"virtual-camera-service\",", 1)
    bp.write_text(t)
    print("patched services/Android.bp")
else:
    print("services/Android.bp already patched")

# 5b. SystemServer.java: start VirtualCameraService after CameraServiceProxy
ss = root / "frameworks/base/services/java/com/android/server/SystemServer.java"
t = ss.read_text()
if "VirtualCameraService" not in t:
    anchor = """                t.traceBegin("StartCameraServiceProxy");
                mSystemServiceManager.startService(CameraServiceProxy.class);
                t.traceEnd();"""
    assert anchor in t, "CameraServiceProxy anchor not found in SystemServer.java"
    addition = anchor + """

                t.traceBegin("StartVirtualCameraService");
                try {
                    mSystemServiceManager.startService(
                            com.android.server.camera.virtual.VirtualCameraService
                                    .Lifecycle.class);
                } catch (Throwable e) {
                    reportWtf("starting VirtualCameraService", e);
                }
                t.traceEnd();"""
    t = t.replace(anchor, addition, 1)
    ss.write_text(t)
    print("patched SystemServer.java")
else:
    print("SystemServer.java already patched")
PYEOF

echo "=== [6/7] sepolicy (system_ext private) ==="
cp "$SCRIPT_DIR/platform/sepolicy/virtual_camera_hal.te" "$SEPOLICY_DEST/"
cp "$SCRIPT_DIR/platform/sepolicy/virtual_camera_service.te" "$SEPOLICY_DEST/"
# service_contexts: create or append (idempotent)
touch "$SEPOLICY_DEST/service_contexts"
while IFS= read -r line; do
    [ -z "$line" ] && continue
    key=$(echo "$line" | awk "{print \$1}")
    grep -q "^$key " "$SEPOLICY_DEST/service_contexts" || echo "$line" >> "$SEPOLICY_DEST/service_contexts"
done < "$SCRIPT_DIR/platform/sepolicy/service_contexts"
# file_contexts: append fragment (idempotent)
grep -q "virtual_camera_hal_exec" "$SEPOLICY_DEST/file_contexts" 2>/dev/null || \
    cat "$SCRIPT_DIR/platform/sepolicy/file_contexts_fragment" >> "$SEPOLICY_DEST/file_contexts"

# Remove stale vendor-side service_contexts entry (provider is system_ext now,
# labeled virtual_camera_provider_service from system_ext service_contexts)
VENDOR_SC="$AOSP_ROOT/device/google/cuttlefish/shared/sepolicy/vendor/service_contexts"
if grep -q "virtual_renderer" "$VENDOR_SC" 2>/dev/null; then
    sed -i "\#virtual_renderer#d" "$VENDOR_SC"
    echo "removed vendor service_contexts entry"
fi

echo "=== [7/7] PRODUCT_PACKAGES ==="
grep -q "VCamProducer" "$DEVICE_MK" || cat >> "$DEVICE_MK" << "EOF"

# Virtual camera platform-AIDL test apps
PRODUCT_PACKAGES += VCamProducer VCamViewer
EOF
# HAL binary already in PRODUCT_PACKAGES from previous integration

echo ""
echo "=== Integration complete ==="
echo "Build: cd $AOSP_ROOT && source build/envsetup.sh && lunch aosp_cf_x86_64_phone-userdebug && m"
