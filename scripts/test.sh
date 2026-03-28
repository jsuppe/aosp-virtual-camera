#!/bin/bash
#
# test.sh - Test Virtual Camera on running Cuttlefish
#
# Usage: ./test.sh [check|install|camera|logs]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VCAM_ROOT="$(dirname "$SCRIPT_DIR")"
ACTION="${1:-check}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }
pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; }

# Ensure ADB is connected
ensure_adb() {
    if ! adb devices | grep -q "127.0.0.1:6520"; then
        info "Connecting to Cuttlefish..."
        adb connect 127.0.0.1:6520 > /dev/null 2>&1 || true
        sleep 1
    fi
    
    if ! adb devices | grep -q "127.0.0.1:6520.*device"; then
        error "Cuttlefish not connected. Is it running?"
        exit 1
    fi
}

case "$ACTION" in
    check)
        info "Checking Virtual Camera installation..."
        echo ""
        ensure_adb
        
        # Check services
        echo "Checking services..."
        if adb shell service list 2>/dev/null | grep -q "virtual_camera"; then
            pass "VirtualCameraService is registered"
        else
            fail "VirtualCameraService not found"
        fi
        
        if adb shell service list 2>/dev/null | grep -q "virtual_camera_manager"; then
            pass "VirtualCameraManager is registered"
        else
            fail "VirtualCameraManager not found"
        fi
        
        # Check HAL
        echo ""
        echo "Checking Camera HAL..."
        if adb shell ls /vendor/bin/hw/ 2>/dev/null | grep -q "camera.provider.*virtual"; then
            pass "Virtual Camera HAL binary exists"
        else
            fail "Virtual Camera HAL binary not found"
        fi
        
        # Check SELinux
        echo ""
        echo "Checking SELinux..."
        SELINUX_MODE=$(adb shell getenforce 2>/dev/null)
        if [ "$SELINUX_MODE" = "Enforcing" ]; then
            info "SELinux is enforcing"
            
            # Check for denials
            DENIALS=$(adb shell dmesg 2>/dev/null | grep -c "avc.*virtual_camera" || true)
            if [ "$DENIALS" -gt 0 ]; then
                warn "Found $DENIALS SELinux denials related to virtual_camera"
                echo "  Run: ./test.sh logs selinux"
            else
                pass "No virtual_camera SELinux denials"
            fi
        else
            warn "SELinux is $SELINUX_MODE (not enforcing)"
        fi
        
        # Check camera list
        echo ""
        echo "Checking camera list..."
        CAMERAS=$(adb shell dumpsys media.camera 2>/dev/null | grep -c "Camera.*:" || true)
        info "Found $CAMERAS cameras"
        ;;
    
    install)
        info "Installing sample renderer app..."
        ensure_adb
        
        APK="$VCAM_ROOT/sample-renderer/app/build/outputs/apk/debug/app-debug.apk"
        
        if [ ! -f "$APK" ]; then
            warn "APK not found. Building..."
            cd "$VCAM_ROOT/sample-renderer"
            ./gradlew assembleDebug
        fi
        
        if [ -f "$APK" ]; then
            adb install -r "$APK"
            info "Installed! Launch 'Golden Cube Camera' from app drawer"
        else
            error "Failed to build APK"
        fi
        ;;
    
    camera)
        info "Opening camera app..."
        ensure_adb
        
        # Launch camera
        adb shell am start -a android.media.action.IMAGE_CAPTURE
        
        info "Camera app launched. Select 'Golden Cube Camera' if available."
        ;;
    
    logs)
        LOG_TYPE="${2:-all}"
        ensure_adb
        
        case "$LOG_TYPE" in
            service)
                info "VirtualCameraService logs:"
                adb logcat -d -s VirtualCameraService VirtualCamera
                ;;
            hal)
                info "Camera HAL logs:"
                adb logcat -d -s VirtualCameraProvider VirtualCameraDevice
                ;;
            selinux)
                info "SELinux denials:"
                adb shell dmesg | grep "avc.*denied" | grep -E "virtual_camera|vcam" | tail -50
                ;;
            renderer)
                info "Renderer app logs:"
                adb logcat -d -s VCamRenderer VulkanRenderer
                ;;
            all)
                info "All virtual camera logs:"
                adb logcat -d | grep -iE "virtualcamera|vcam|golden.cube" | tail -100
                ;;
            *)
                echo "Usage: $0 logs [service|hal|selinux|renderer|all]"
                ;;
        esac
        ;;
    
    watch)
        info "Watching logs (Ctrl+C to stop)..."
        ensure_adb
        adb logcat -v time | grep -iE "virtualcamera|vcam|golden.cube"
        ;;
    
    *)
        echo "Usage: $0 [check|install|camera|logs|watch]"
        echo ""
        echo "  check   - Verify virtual camera is installed correctly"
        echo "  install - Install sample renderer app"
        echo "  camera  - Open camera app"
        echo "  logs    - View logs (service|hal|selinux|renderer|all)"
        echo "  watch   - Watch logs in real-time"
        exit 1
        ;;
esac
