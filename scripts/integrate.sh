#!/bin/bash
#
# Virtual Camera HAL Integration Script
# 
# Applies all 4 required changes to an AOSP tree:
# 1. VINTF manifest
# 2. SELinux service_contexts  
# 3. SELinux file_contexts
# 4. init.rc (included in Android.bp)
#
# Usage: ./integrate.sh /path/to/aosp [device_path]
#
# Example for Cuttlefish:
#   ./integrate.sh /mnt/micron/aosp device/google/cuttlefish
#

set -e

AOSP_ROOT="${1:?Usage: $0 <aosp_root> [device_path]}"
DEVICE_PATH="${2:-device/google/cuttlefish}"  # Default to Cuttlefish

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HAL_DEST="$AOSP_ROOT/hardware/interfaces/camera/provider/virtual"
SEPOLICY_DIR="$AOSP_ROOT/$DEVICE_PATH/shared/camera/sepolicy"

echo "=== Virtual Camera HAL Integration ==="
echo "AOSP Root: $AOSP_ROOT"
echo "Device: $DEVICE_PATH"
echo "HAL Destination: $HAL_DEST"
echo ""

# Verify AOSP root
if [[ ! -f "$AOSP_ROOT/build/envsetup.sh" ]]; then
    echo "ERROR: $AOSP_ROOT doesn't look like an AOSP tree"
    exit 1
fi

# 1. Copy HAL source + v2 shared memory headers
echo "[1/4] Copying HAL source..."
mkdir -p "$HAL_DEST"
cp "$SCRIPT_DIR/../hal/"*.cpp "$HAL_DEST/"
cp "$SCRIPT_DIR/../hal/"*.h "$HAL_DEST/"
cp "$SCRIPT_DIR/../hal/Android.bp" "$HAL_DEST/"

# Copy v2 shared memory headers (included by HAL via ../v2-shared-memory)
V2_DEST="$(dirname "$HAL_DEST")/v2-shared-memory"
mkdir -p "$V2_DEST"
cp "$SCRIPT_DIR/../v2-shared-memory/"*.h "$V2_DEST/"

# Create VINTF manifest
cat > "$HAL_DEST/android.hardware.camera.provider-virtual-service.xml" << 'EOF'
<manifest version="1.0" type="device">
    <hal format="aidl">
        <name>android.hardware.camera.provider</name>
        <version>2</version>
        <fqname>ICameraProvider/virtual_renderer/0</fqname>
    </hal>
</manifest>
EOF

# Create init.rc
cat > "$HAL_DEST/android.hardware.camera.provider-virtual-service.rc" << 'EOF'
service vendor.camera.provider-virtual /vendor/bin/hw/android.hardware.camera.provider-virtual-service
    class hal
    user cameraserver
    group audio camera input drmrpc
    ioprio rt 4
    capabilities SYS_NICE
    task_profiles CameraServiceCapacity MaxPerformance
EOF

echo "   ✓ HAL source copied"

# 2. Add service_contexts
echo "[2/4] Adding SELinux service_contexts..."
SERVICE_CTX="android.hardware.camera.provider.ICameraProvider/virtual_renderer/0 u:object_r:hal_camera_service:s0"

if [[ -d "$SEPOLICY_DIR" ]]; then
    if ! grep -q "virtual_renderer" "$SEPOLICY_DIR/service_contexts" 2>/dev/null; then
        echo "$SERVICE_CTX" >> "$SEPOLICY_DIR/service_contexts"
        echo "   ✓ Added to $SEPOLICY_DIR/service_contexts"
    else
        echo "   ✓ Already present in service_contexts"
    fi
else
    echo "   ⚠ SEPolicy dir not found: $SEPOLICY_DIR"
    echo "   → Add manually: $SERVICE_CTX"
fi

# 3. Add file_contexts
echo "[3/4] Adding SELinux file_contexts..."
FILE_CTX="/vendor/bin/hw/android\\.hardware\\.camera\\.provider-virtual-service u:object_r:hal_camera_default_exec:s0"

if [[ -d "$SEPOLICY_DIR" ]]; then
    if ! grep -q "virtual-service" "$SEPOLICY_DIR/file_contexts" 2>/dev/null; then
        echo "$FILE_CTX" >> "$SEPOLICY_DIR/file_contexts"
        echo "   ✓ Added to $SEPOLICY_DIR/file_contexts"
    else
        echo "   ✓ Already present in file_contexts"
    fi
else
    echo "   ⚠ SEPolicy dir not found: $SEPOLICY_DIR"
    echo "   → Add manually: $FILE_CTX"
fi

# 4. Verify init.rc is in Android.bp
echo "[4/4] Verifying Android.bp includes init.rc and vintf_fragments..."
if grep -q "init_rc" "$HAL_DEST/Android.bp" && grep -q "vintf_fragments" "$HAL_DEST/Android.bp"; then
    echo "   ✓ Android.bp includes init.rc and vintf_fragments"
else
    echo "   ⚠ Android.bp may need manual update"
fi

echo ""
echo "=== Integration Complete ==="
echo ""
echo "Next steps:"
echo "1. Add to device makefile:"
echo "   PRODUCT_PACKAGES += android.hardware.camera.provider-virtual-service"
echo ""
echo "2. Build:"
echo "   cd $AOSP_ROOT"
echo "   source build/envsetup.sh"
echo "   lunch <target>"
echo "   m android.hardware.camera.provider-virtual-service"
echo "   m  # Full rebuild for SELinux changes"
echo ""
echo "3. Verify after boot:"
echo "   adb shell service list | grep virtual_renderer"
