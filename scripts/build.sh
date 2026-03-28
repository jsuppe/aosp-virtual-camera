#!/bin/bash
#
# build.sh - Build AOSP with Virtual Camera
#
# Usage: ./build.sh [full|modules|sample]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AOSP_ROOT="${AOSP_ROOT:-/mnt/micron/aosp}"
BUILD_TYPE="${1:-full}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

cd "$AOSP_ROOT"

# Setup environment
info "Setting up build environment..."
source build/envsetup.sh
lunch aosp_cf_x86_64_only_phone-userdebug

case "$BUILD_TYPE" in
    full)
        info "Building full AOSP (this will take a while)..."
        m -j$(nproc)
        ;;
    
    modules)
        info "Building only virtual camera modules..."
        m VirtualCameraService \
          android.hardware.camera.provider-virtual-service \
          -j$(nproc)
        ;;
    
    sample)
        info "Building sample renderer app..."
        VCAM_ROOT="$(dirname "$SCRIPT_DIR")"
        cd "$VCAM_ROOT/sample-renderer"
        
        if [ ! -f "gradlew" ]; then
            warn "Gradle wrapper not found. Creating..."
            gradle wrapper
        fi
        
        ./gradlew assembleDebug
        
        APK="app/build/outputs/apk/debug/app-debug.apk"
        if [ -f "$APK" ]; then
            info "APK built: $APK"
            echo ""
            echo "Install with: adb install $APK"
        fi
        ;;
    
    *)
        echo "Usage: $0 [full|modules|sample]"
        echo ""
        echo "  full    - Build entire AOSP (slow, ~1-2 hours)"
        echo "  modules - Build only virtual camera modules (fast)"
        echo "  sample  - Build sample renderer app"
        exit 1
        ;;
esac

info "Build complete!"
