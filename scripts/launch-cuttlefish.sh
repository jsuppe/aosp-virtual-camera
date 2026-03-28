#!/bin/bash
#
# launch-cuttlefish.sh - Launch Cuttlefish VM
#
# Usage: ./launch-cuttlefish.sh [start|stop|status|vnc|web]
#

set -e

AOSP_ROOT="${AOSP_ROOT:-/mnt/micron/aosp}"
ACTION="${1:-start}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Check prerequisites
check_prereqs() {
    # Check groups
    if ! groups | grep -q cvdnetwork; then
        error "User not in cvdnetwork group. Run: sudo usermod -aG cvdnetwork \$USER"
    fi
    
    if ! groups | grep -q kvm; then
        error "User not in kvm group. Run: sudo usermod -aG kvm \$USER"
    fi
    
    # Check KVM
    if [ ! -e /dev/kvm ]; then
        error "KVM not available. Enable virtualization in BIOS."
    fi
}

case "$ACTION" in
    start)
        check_prereqs
        
        cd "$AOSP_ROOT"
        source build/envsetup.sh
        lunch aosp_cf_x86_64_only_phone-userdebug
        
        info "Launching Cuttlefish..."
        echo ""
        echo -e "${CYAN}Access methods:${NC}"
        echo "  WebRTC: https://localhost:8443"
        echo "  ADB:    adb connect 127.0.0.1:6520"
        echo "  VNC:    localhost:6444"
        echo ""
        
        launch_cvd \
            --start_webrtc=true \
            --webrtc_public_ip=127.0.0.1 \
            --cpus=4 \
            --memory_mb=4096
        ;;
    
    stop)
        info "Stopping Cuttlefish..."
        stop_cvd || true
        pkill -f crosvm || true
        info "Stopped"
        ;;
    
    status)
        if pgrep -f crosvm > /dev/null; then
            info "Cuttlefish is running"
            echo ""
            echo "Processes:"
            pgrep -a -f "crosvm|cvd"
            echo ""
            echo "ADB devices:"
            adb devices
        else
            warn "Cuttlefish is not running"
        fi
        ;;
    
    vnc)
        info "Opening VNC viewer..."
        if command -v vncviewer &> /dev/null; then
            vncviewer localhost:6444 &
        elif command -v vinagre &> /dev/null; then
            vinagre localhost:6444 &
        else
            warn "No VNC viewer found. Connect to localhost:6444 manually."
        fi
        ;;
    
    web)
        info "Opening WebRTC viewer..."
        if command -v xdg-open &> /dev/null; then
            xdg-open "https://localhost:8443" &
        elif command -v open &> /dev/null; then
            open "https://localhost:8443" &
        else
            echo "Open in browser: https://localhost:8443"
        fi
        ;;
    
    adb)
        info "Connecting ADB..."
        adb connect 127.0.0.1:6520
        adb devices
        ;;
    
    shell)
        adb -s 127.0.0.1:6520 shell
        ;;
    
    *)
        echo "Usage: $0 [start|stop|status|vnc|web|adb|shell]"
        echo ""
        echo "  start  - Launch Cuttlefish VM"
        echo "  stop   - Stop Cuttlefish VM"
        echo "  status - Check if running"
        echo "  vnc    - Open VNC viewer"
        echo "  web    - Open WebRTC in browser"
        echo "  adb    - Connect ADB"
        echo "  shell  - Open ADB shell"
        exit 1
        ;;
esac
