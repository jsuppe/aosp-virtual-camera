#!/usr/bin/env bash
# Read-only checks for the A13 reference APEX's userdebug deployment loop.
# Does not root, remount, install, flash, reboot, or change device settings.
set -euo pipefail

usage() {
    echo "Usage: bash $0 SERIAL [EXPECTED_SDK [EXPECTED_ABI]]"
    echo "Defaults: SDK 33, ABI x86_64 (the current A13 Cuttlefish artifact)."
    echo "Override only for an artifact actually built for that SDK/ABI."
    echo "Exit: 0 = checked prerequisites present; 1 = blocked; 2 = usage/ADB error."
    echo "This does not validate signing, native-library compatibility, or frame flow."
}

if [[ ${1:-} == --help || ${1:-} == -h ]]; then usage; exit 0; fi
if (( $# < 1 || $# > 3 )); then usage >&2; exit 2; fi
serial=$1
expected_sdk=${2:-33}
expected_abi=${3:-x86_64}
if [[ ! $expected_sdk =~ ^[0-9]+$ || ! $expected_abi =~ ^[a-zA-Z0-9_-]+$ ]]; then
    usage >&2
    exit 2
fi
command -v adb >/dev/null || { echo 'ERROR: adb is not on PATH' >&2; exit 2; }
adb_cmd=(adb -s "$serial")
if ! state=$("${adb_cmd[@]}" get-state 2>/dev/null) || [[ $state != device ]]; then
    echo "ERROR: selected device is not connected and authorized: $serial" >&2
    exit 2
fi

prop() { "${adb_cmd[@]}" shell getprop "$1" | tr -d '\r'; }
blocked=0
pass() { echo "PASS: $*"; }
fail() { echo "BLOCKED: $*"; blocked=$((blocked + 1)); }

sdk=$(prop ro.build.version.sdk)
abis=$(prop ro.product.cpu.abilist)
debuggable=$(prop ro.debuggable)
echo "Device: $serial"
echo "Fingerprint: $(prop ro.build.fingerprint)"
echo "Build type: $(prop ro.build.type); SDK: $sdk; ABIs: $abis"
echo "SELinux: $("${adb_cmd[@]}" shell getenforce | tr -d '\r')"
echo "Expected artifact: SDK $expected_sdk, ABI $expected_abi"

if [[ $sdk == "$expected_sdk" ]]; then
    pass 'reference SDK matches'
else
    fail "SDK mismatch: build/qualify the adapter for SDK $sdk; do not deploy this reference artifact"
fi
if [[ ,$abis, == *,$expected_abi,* ]]; then
    pass 'required ABI is supported'
else
    fail "ABI mismatch: build an artifact for one of $abis"
fi
if [[ $debuggable == 1 ]]; then
    pass 'debuggable build (root/remount still require separate verification)'
else
    fail 'not a debuggable build; the reference vendor-replacement loop is unavailable; use an OEM-integrated image/OTA'
fi

if ! services=$("${adb_cmd[@]}" shell service list); then
    echo 'ERROR: could not inspect Binder services' >&2
    exit 2
fi
if grep -Eq '[[:space:]]virtual_camera:' <<< "$services"; then
    pass 'platform virtual_camera broker is registered'
else
    fail 'platform virtual_camera broker is missing; requires base-image integration'
fi
if grep -Fq 'android.hardware.virtualcamera.hal.IVirtualCameraHal/default:' <<< "$services"; then
    pass 'custom vendor HAL boundary is registered'
else
    fail 'custom vendor HAL boundary is not registered; integrate/diagnose the initial HAL and VINTF/policy'
fi
if grep -Fq 'android.hardware.camera.provider.ICameraProvider/virtual_renderer/0:' <<< "$services"; then
    pass 'virtual camera provider is registered'
else
    fail 'virtual camera provider is not registered'
fi

if [[ $expected_abi == *64* ]]; then libdir=lib64; else libdir=lib; fi
if "${adb_cmd[@]}" shell test -f "/system_ext/$libdir/libvirtualcamera_relay_jni.so"; then
    pass 'platform JNI relay exists'
else
    fail "platform JNI relay missing at /system_ext/$libdir; requires base-image integration"
fi
if "${adb_cmd[@]}" shell test -f /vendor/apex/com.android.hardware.camera.provider.virtual.apex; then
    pass 'reference vendor APEX exists'
else
    fail 'reference vendor APEX is not installed in the base image'
fi

if (( blocked > 0 )); then
    echo "RESULT: BLOCKED ($blocked prerequisite checks failed). No device changes made."
    exit 1
fi
echo 'RESULT: checked reference deployment prerequisites present.'
echo 'Next: verify signatures, dependency compatibility, recovery plan, and the exact artifact before deployment.'
