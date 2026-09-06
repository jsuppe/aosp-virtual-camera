#!/bin/bash
#
# integrate-a13-platform.sh — Integrate the platform-AIDL virtual camera into
# an Android 13 AOSP tree (Cuttlefish).
#
# Idempotent: safe to re-run after repo changes.
#
# What it does:
#  1. HAL sources (core + v1 adapter) -> hardware/interfaces/camera/provider/virtual/
#     with the Android.bp/rc/VINTF variant selected by MODE (see below)
#  2. Platform AIDL lib -> .../virtual/platform-aidl/
#  3. VirtualCameraService java lib -> .../virtual/platform-service/
#  4. Test apps -> .../virtual/apps/{VCamProducer,VCamViewer}
#  5. frameworks/base: services static_libs += virtual-camera-service,
#     SystemServer registers VirtualCameraService.Lifecycle
#  6. sepolicy -> device/google/cuttlefish/shared/sepolicy/{system_ext/private,vendor}/
#  7. PRODUCT_PACKAGES += apps (device/google/cuttlefish/shared/device.mk)
#
# Usage: ./integrate-a13-platform.sh <aosp_root> [apex|system_ext]
#
#   apex        (default) the shipping architecture: vendor APEX HAL in the
#               hal_camera_default domain + frozen android.hardware.virtualcamera.hal
#               boundary. Real SELinux policy (device runs enforcing).
#   system_ext  iteration-1 prototype: HAL on /system_ext owning the relay
#               BufferQueues (VCAM_AIDL_SOURCE). Prototype-grade policy; run
#               the VM permissive.
#
# Switching modes is supported: each mode removes the other's policy entries.

set -e

AOSP_ROOT="${1:?Usage: $0 <aosp_root> [apex|system_ext]}"
MODE="${2:-apex}"
case "$MODE" in apex|system_ext) ;; *) echo "unknown mode: $MODE (apex|system_ext)"; exit 1;; esac
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"   # repo root
HAL_DEST="$AOSP_ROOT/hardware/interfaces/camera/provider/virtual"
SEPOLICY_DEST="$AOSP_ROOT/device/google/cuttlefish/shared/sepolicy/system_ext/private"
VENDOR_SEPOLICY_DEST="$AOSP_ROOT/device/google/cuttlefish/shared/sepolicy/vendor"
DEVICE_MK="$AOSP_ROOT/device/google/cuttlefish/shared/device.mk"
echo "mode: $MODE"

[ -f "$AOSP_ROOT/build/envsetup.sh" ] || { echo "not an AOSP tree: $AOSP_ROOT"; exit 1; }

echo "=== [0/7] Stable AIDL boundary + platform JNI + APEX (vendor packaging) ==="
rm -rf "$HAL_DEST/stable-aidl" "$HAL_DEST/platform-jni" "$HAL_DEST/apex"
cp -r "$SCRIPT_DIR/stable-aidl"   "$HAL_DEST/stable-aidl"
cp -r "$SCRIPT_DIR/platform-jni"  "$HAL_DEST/platform-jni"
cp -r "$SCRIPT_DIR/apex"          "$HAL_DEST/apex"

echo "=== [1/7] HAL sources ($MODE variant) ==="
mkdir -p "$HAL_DEST/core" "$HAL_DEST/aidl"
cp "$SCRIPT_DIR"/hal/core/*.cpp "$SCRIPT_DIR"/hal/core/*.h "$HAL_DEST/core/"
cp "$SCRIPT_DIR"/hal/aidl-v1/*.cpp "$SCRIPT_DIR"/hal/aidl-v1/*.h "$HAL_DEST/aidl/"
if [ "$MODE" = apex ]; then
    VV="$SCRIPT_DIR/platform/vendor-variant"
    cp "$VV/core.Android.bp"    "$HAL_DEST/core/Android.bp"
    cp "$VV/aidl-v1.Android.bp" "$HAL_DEST/aidl/Android.bp"
    cp "$VV/android.hardware.camera.provider-virtual-service.rc"  "$HAL_DEST/aidl/"
    cp "$VV/android.hardware.camera.provider-virtual-service.xml" "$HAL_DEST/aidl/"
else
    cp "$SCRIPT_DIR/platform/bp/core.Android.bp"      "$HAL_DEST/core/Android.bp"
    cp "$SCRIPT_DIR/platform/bp/aidl-v1.Android.bp"   "$HAL_DEST/aidl/Android.bp"
    cp "$SCRIPT_DIR/platform/rc/android.hardware.camera.provider-virtual-service.rc"   "$HAL_DEST/aidl/"
    cp "$SCRIPT_DIR/platform/vintf/android.hardware.camera.provider-virtual-service.xml" "$HAL_DEST/aidl/"
fi
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

echo "=== [6/7] sepolicy ($MODE) ==="
SEP="$SCRIPT_DIR/platform/sepolicy"
VENDOR_SC="$VENDOR_SEPOLICY_DEST/service_contexts"
VENDOR_FC="$VENDOR_SEPOLICY_DEST/file_contexts"

# helper: append each non-empty line of $1 to $2 unless its first field is present
append_contexts() {
    touch "$2"
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        key=$(echo "$line" | awk '{print $1}')
        grep -q "^$key " "$2" || echo "$line" >> "$2"
    done < "$1"
}
# helper: delete lines whose first field is in $1 from $2
remove_contexts() {
    [ -f "$2" ] || return 0
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        key=$(echo "$line" | awk '{print $1}')
        sed -i "\#^$key #d" "$2"
    done < "$1"
}

# Both modes: VirtualCameraService's two system_server binder names.
cp "$SEP/virtual_camera_service.te" "$SEPOLICY_DEST/"
append_contexts "$SEP/system_ext_service_contexts" "$SEPOLICY_DEST/service_contexts"

if [ "$MODE" = apex ]; then
    # Vendor side: IVirtualCameraHal label + system_server<->HAL binder rules,
    # provider name labeled hal_camera_service (registerable by hal_camera_default).
    cp "$SEP/vendor/hal_camera_virtual.te" "$VENDOR_SEPOLICY_DEST/"
    append_contexts "$SEP/vendor/service_contexts" "$VENDOR_SC"
    # Loose vendor binary (non-APEX vendor build) gets the same domain as the APEX one.
    grep -q 'provider-virtual-service u:object_r:hal_camera_default_exec' "$VENDOR_FC" || \
        echo '/vendor/bin/hw/android\.hardware\.camera\.provider-virtual-service u:object_r:hal_camera_default_exec:s0' >> "$VENDOR_FC"
    # Drop the system_ext-relay prototype's HAL domain + provider label if present:
    # a second label for the same service name would shadow the vendor one.
    rm -f "$SEPOLICY_DEST/virtual_camera_hal.te"
    sed -i '\#virtual_renderer#d' "$SEPOLICY_DEST/service_contexts"
    sed -i '\#virtual_camera_hal_exec#d' "$SEPOLICY_DEST/file_contexts" 2>/dev/null || true
else
    cp "$SEP/virtual_camera_hal.te" "$SEPOLICY_DEST/"
    append_contexts "$SEP/service_contexts" "$SEPOLICY_DEST/service_contexts"
    grep -q "virtual_camera_hal_exec" "$SEPOLICY_DEST/file_contexts" 2>/dev/null || \
        cat "$SEP/file_contexts_fragment" >> "$SEPOLICY_DEST/file_contexts"
    # Remove the vendor-side entries (provider is system_ext in this mode).
    rm -f "$VENDOR_SEPOLICY_DEST/hal_camera_virtual.te"
    remove_contexts "$SEP/vendor/service_contexts" "$VENDOR_SC"
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
[ "$MODE" = apex ] && echo "Incremental after policy/HAL edits: m com.android.hardware.camera.provider.virtual selinux_policy"
