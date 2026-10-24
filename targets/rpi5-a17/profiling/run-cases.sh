#!/system/bin/sh
# Run only on the selected matched Pi after installing diagnostic APEX/APK.
# Capture uses device CLOCK_MONOTONIC timestamps; no screen recording.
set -eu
[ "$(getprop ro.product.device)" = rpi5 ]
[ "$(getprop ro.build.version.sdk)" = 37 ]
: "${EXPECTED_HAL_SHA:?Pass the verified installed diagnostic HAL SHA256}"
: "${EXPECTED_IMPL_SHA:?Pass the verified diagnostic implementation library SHA256}"
actual_sha=$(sha256sum /apex/com.android.hardware.camera.provider.virtual/bin/hw/android.hardware.camera.provider-virtual-service | cut -d ' ' -f1)
[ "$actual_sha" = "$EXPECTED_HAL_SHA" ]
actual_impl=$(sha256sum /apex/com.android.hardware.camera.provider.virtual/lib64/android.hardware.camera.provider-V1-virtual-impl.so | cut -d ' ' -f1)
[ "$actual_impl" = "$EXPECTED_IMPL_SHA" ]
su 0 mkdir -p /data/vendor/vcam-profile
su 0 chown cameraserver:shell /data/vendor/vcam-profile
su 0 chmod 0770 /data/vendor/vcam-profile
app=com.example.vcamproducer
trap 'su 0 setprop vendor.vcam.profile 0' EXIT
for case_id in 201 202 203 204; do
    fps=120; mirror=true; clock=true
    case "$case_id" in
        201) fps=60;;
        203) mirror=false;;
        204) mirror=false; clock=false;;
    esac
    echo "VCAM_PROFILE_BEGIN run=$case_id fps=$fps mirror=$mirror clock=$clock"
    cat /sys/class/thermal/thermal_zone0/temp
    su 0 setprop vendor.vcam.profile "$case_id"
    am start -W -f 0x10008000 -n "$app/.SplitDemoActivity" --ei fps "$fps" --ei width 1920 \
        --ei profile_run "$case_id" --ez mirror "$mirror" --ez clock "$clock"
    sleep 70
    am start -W -f 0x10008000 -n "$app/.MainActivity"
    sleep 3
    echo "VCAM_PROFILE_END run=$case_id"
    cat /sys/class/thermal/thermal_zone0/temp
done
su 0 setprop vendor.vcam.profile 0
am start -W -f 0x10008000 -n "$app/.SplitDemoActivity" --ei fps 60 --ei width 1920
echo VCAM_PROFILE_ALL_DONE
