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
echo "--- SELinux: $($ADB shell getenforce) (apex mode ships real policy; expect Enforcing)"
$ADB shell "dmesg -c >/dev/null" 2>/dev/null || true   # baseline for the denial count below
$ADB shell settings put global hidden_api_policy 1 2>/dev/null || true

echo "--- system services ---"
$ADB shell "service list | grep -E \"virtual_camera\"" || echo "!! virtual_camera services missing"
echo "--- HAL process ---"
$ADB shell "ps -A | grep provider-virtual" || echo "!! HAL not running"
echo "--- camera enumeration ---"
$ADB shell "dumpsys media.camera | grep -E \"Number of camera|Device.*maps\"" | head -6

echo ""
echo "=== Step 1: start producer service ==="
$ADB shell pm grant com.example.vcamproducer android.permission.CAMERA 2>/dev/null || true
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
echo "--- boundary (apex mode: JNI pump -> IVirtualCameraHal.queueFrame; system_ext mode: VCamAidlSource) ---"
$ADB logcat -d -s VCamRelayJni:* VCamStableHal:* VCamAidlSource:* VirtualCameraSession:* | grep -E "Pushed|queueFrame|AIDL|Filled|configured" | tail -8
echo "--- viewer frames ---"
$ADB logcat -d -s VCamViewer:* | grep -E "Camera opened|RECEIVED|previewing|error" | tail -6

echo ""
echo "--- screenshot ---"
$ADB exec-out screencap -p > /home/melchior/vcam_validation.png && echo "saved ~/vcam_validation.png"

echo ""
echo "--- frame pacing (viewer-side delivery rate over 5 s; HAL paces to AE_TARGET_FPS_RANGE, default 30) ---"
recv_count() { $ADB logcat -d -s VCamViewer:* | grep "RECEIVED" | tail -1 | sed -E 's/.*RECEIVED ([0-9]+) frames.*/\1/'; }
R0=$(recv_count); sleep 5; R1=$(recv_count)
if [ -n "$R0" ] && [ -n "$R1" ]; then echo "viewer received $((R1-R0)) frames in 5 s = ~$(( (R1-R0) / 5 )) fps"; else echo "no viewer counters"; fi
echo "--- latency (A3: producer BufferQueue timestamp carried in vendor tag com.virtualcamera.producerTimestampNs) ---"
$ADB logcat -d -s VCamViewer:* | grep -E "latency producer" | tail -2 | sed -E 's/.*RECEIVED/RECEIVED/'
echo "--- boundary mode ---"
$ADB logcat -d -s VCamRelayJni:* VCamStableHal:* | grep -E "Connected to|Pushed .*fenced|\[fenced\]|native fences" | tail -3
$ADB logcat -d -s VCamGpuCompositor:* | grep -E "native fences" | tail -1

echo ""
echo "=== Step 3: YUV consumer (ImageReader YUV_420_888 + preview; HAL GPU-YUV path) ==="
$ADB shell am force-stop com.example.vcamviewer
sleep 1
$ADB shell am start -n com.example.vcamviewer/.MainActivity --ez yuv true >/dev/null
sleep 12
$ADB logcat -d -s VCamViewer:* | grep -E "YUV mode|YUV frame" | tail -3
echo "--- HAL fill-path mix (expect gpu-yuv > 0, cpu-yuv 0) ---"
$ADB logcat -d -s VirtualCameraSession:* | grep -E "Processed .* frames: " | tail -1 | sed -E 's/.*Processed/Processed/'

echo ""
echo "=== Step 4: JPEG still (BLOB stream, TEMPLATE_STILL_CAPTURE; HAL JpegEncoder) ==="
$ADB shell am force-stop com.example.vcamviewer
sleep 1
$ADB shell am start -n com.example.vcamviewer/.MainActivity --ez jpeg true >/dev/null
sleep 10
$ADB logcat -d -s VCamViewer:* VirtualCameraSession:* | grep -E "JPEG" | tail -4 | sed -E 's/^[0-9:. -]+[0-9]+ +[0-9]+ [IEW] //'

echo ""
echo "--- SELinux denials during the run (virtual-camera related) ---"
AVC=$($ADB shell "dmesg | grep -E 'avc: *denied'" | grep -E 'hal_camera_default|virtual_?camera|virtualcamera|vcamproducer|vcamviewer' || true)
if [ -n "$AVC" ]; then echo "$AVC" | sed -E 's/^.*avc: /avc: /' | sort | uniq -c | sort -rn | head -20; else echo "none"; fi

echo ""
PROD=$($ADB logcat -d -s VCamProducer:* | grep -c "PRODUCED" || true)
RECV=$($ADB logcat -d -s VCamViewer:* | grep -c "RECEIVED" || true)
FILL=$($ADB logcat -d -s VCamStableHal:* VCamAidlSource:* | grep -cE "queueFrame: .* frames received|Filled" || true)
echo "Summary: producer-log-batches=$PROD hal-fill-batches=$FILL viewer-log-batches=$RECV"
if [ "$PROD" -gt 0 ] && [ "$FILL" -gt 0 ] && [ "$RECV" -gt 0 ]; then
    echo "RESULT: END-TO-END AIDL FRAME FLOW VALIDATED ✅"
else
    echo "RESULT: INCOMPLETE — check logs above ❌"
fi
