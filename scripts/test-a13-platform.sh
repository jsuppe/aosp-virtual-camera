#!/bin/bash
#
# test-a13-platform.sh — Validate the platform-AIDL producer->camera flow on a
# booted A13 Cuttlefish VM.
#
# Scenario:
#   1. VCamProducerService starts, registers as producer via IVirtualCameraService
#   2. VCamViewer opens virtual camera "100" (Camera2) and renders to a surface
#   3. Frames flow producer app -> Surface/BufferQueue -> HAL -> camera framework -> viewer
#
# Usage: ./test-a13-platform.sh [adb_serial]

set -e
ADB="adb -s ${1:-127.0.0.1:6520}"

echo "=== Device state ==="
$ADB wait-for-device
$ADB root >/dev/null 2>&1 || true
sleep 2
$ADB shell setenforce 0 2>/dev/null || true   # prototype: permissive (sepolicy hardening is follow-up work)
$ADB shell settings put global hidden_api_policy 1 2>/dev/null || true

echo "--- system services ---"
$ADB shell "service list | grep -E \"virtual_camera\"" || echo "!! virtual_camera services missing"
echo "--- HAL process ---"
$ADB shell "ps -A | grep provider-virtual" || echo "!! HAL not running"
echo "--- camera enumeration ---"
$ADB shell "dumpsys media.camera | grep -E \"Number of camera|Device.*maps\"" | head -6

echo ""
echo "=== Step 1: start producer service ==="
$ADB shell am start-foreground-service com.example.vcamproducer/.VCamProducerService
sleep 3
$ADB logcat -d -s VCamProducer:* VirtualCameraService:* | tail -10

echo ""
echo "=== Step 2: launch viewer activity ==="
$ADB shell pm grant com.example.vcamviewer android.permission.CAMERA || true
$ADB shell am start -n com.example.vcamviewer/.MainActivity
echo "waiting 15s for frames to flow..."
sleep 15

echo ""
echo "=== Validation ==="
echo "--- producer frames ---"
$ADB logcat -d -s VCamProducer:* | grep -E "REGISTERED|onStreamsConfigured|PRODUCED" | tail -6
echo "--- service relay ---"
$ADB logcat -d -s VirtualCameraService:* | tail -6
echo "--- HAL AIDL source ---"
$ADB logcat -d -s VCamAidlSource:* VirtualCameraSession:* | grep -E "AIDL|Filled|configured" | tail -8
echo "--- viewer frames ---"
$ADB logcat -d -s VCamViewer:* | grep -E "Camera opened|RECEIVED|previewing|error" | tail -6

echo ""
echo "--- screenshot ---"
$ADB exec-out screencap -p > /home/melchior/vcam_validation.png && echo "saved ~/vcam_validation.png"

echo ""
PROD=$($ADB logcat -d -s VCamProducer:* | grep -c "PRODUCED" || true)
RECV=$($ADB logcat -d -s VCamViewer:* | grep -c "RECEIVED" || true)
FILL=$($ADB logcat -d -s VCamAidlSource:* | grep -c "Filled" || true)
echo "Summary: producer-log-batches=$PROD hal-fill-batches=$FILL viewer-log-batches=$RECV"
if [ "$PROD" -gt 0 ] && [ "$FILL" -gt 0 ] && [ "$RECV" -gt 0 ]; then
    echo "RESULT: END-TO-END AIDL FRAME FLOW VALIDATED ✅"
else
    echo "RESULT: INCOMPLETE — check logs above ❌"
fi
