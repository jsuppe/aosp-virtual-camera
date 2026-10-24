#!/system/bin/sh
set -eu
[ "$(getprop ro.product.device)" = rpi5 ]
[ "$(getprop ro.build.version.sdk)" = 37 ]
[ "$(sha256sum /apex/com.android.hardware.camera.provider.virtual/lib64/android.hardware.camera.provider-V1-virtual-impl.so | cut -d " " -f1)" = 693a8a2eecd9338be0d679d35f7ae5268640aca369637fa476649634eb535d94 ]
dir=/data/local/tmp/vcam-deadline
app=com.example.vcamproducer
trap 'sh "$dir/restore.sh"' EXIT
for run in 401 402 403 404 405; do
 minimal=false; budget=1
 case "$run" in 402|403|405) minimal=true;; esac
 case "$run" in 403|404) budget=2;; esac
 am force-stop "$app"
 sleep 2
 su 0 setprop vendor.vcam.profile "$run"
 su 0 setprop vendor.vcam.profile.deadline "$budget"
 echo "BEGIN run=$run minimal=$minimal budget=$budget"
 cat /sys/class/thermal/thermal_zone0/temp
 am start -W -f 0x10008000 -n "$app/.SplitDemoActivity" --ei fps 120 --ei width 1920 --ei profile_run "$run" --ez mirror false --ez clock false --ez minimal_scene "$minimal"
 sleep 70
 # Kernel trace is separate from the fixed60s throughput window.
 su 0 sh "$dir/trace-window.sh" "$run"
 am start -W -f 0x10008000 -n "$app/.MainActivity"
 sleep 3
 echo "END run=$run"
 cat /sys/class/thermal/thermal_zone0/temp
done
su 0 setprop vendor.vcam.profile 0
su 0 setprop vendor.vcam.profile.deadline 1
am force-stop "$app"
am start -W -f 0x10008000 -n "$app/.SplitDemoActivity" --ei fps 60 --ei width 1920
echo VCAM_DEADLINE_ALL_DONE
