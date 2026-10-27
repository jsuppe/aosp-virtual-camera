#!/system/bin/sh
# Run on matched Pi with diagnostic overlay already installed. No persistent changes.
set -eu
run=$1; fps=$2; clear=$3; left=$4; sized=$5; cached=$6; reuse=$7
app=com.example.vcamproducer
dir=/data/local/tmp/vcam-4k-pocs
mkdir -p "$dir/$run"
am force-stop "$app"
sleep 1
setprop vendor.vcam.profile "$run"
setprop vendor.vcam.profile.clear "$clear"
setprop vendor.vcam.profile.deadline 1
am start -W -f 0x10008000 -n "$app/.SplitDemoActivity" --ei width 3840 --ei fps "$fps" --ei profile_run "$run" --ez left_surface "$left" --ez preview_sized "$sized" --ez cached_clock "$cached" --ez render_once "$reuse" --ez surface_preview true --ez mirror true --ez clock true
snapshot() {
 cat /proc/uptime
 cat /sys/class/thermal/thermal_zone0/temp
 cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq
 cat /sys/kernel/debug/dri/0/measure_clock
}
snapshot > "$dir/$run/system-before.txt"
# Analysis uses first producer timestamp + 15s to +75s; extra tail proves coverage.
sleep 82
snapshot > "$dir/$run/system-after.txt"
sh /data/local/tmp/vcam-investigation/trace-window.sh "$run"
dumpsys media.camera > "$dir/$run/camera.txt"
dumpsys SurfaceFlinger > "$dir/$run/sf.txt"
am start -W -f 0x10008000 -n "$app/.MainActivity"
sleep 2
# Gracefully retire producer to flush bounded profiling samples; then archive.
stop_result=$(am stopservice -n "$app/.VCamProducerService" 2>&1) || stop_status=$?
printf "%s\n" "$stop_result"
# A17 am returns1 when a service was stopped; verify the exact success text.
case "$stop_result" in *"Service stopped"*) ;; *) exit 2;; esac
sleep 2
cp /data/user/0/$app/files/vcam-profile/*"-$run-"*.csv "$dir/$run/"
cp /data/vendor/vcam-profile/hal-"$run"-*.csv "$dir/$run/"
cp /data/local/tmp/vcam-investigation/trace-"$run"* "$dir/$run/"
logcat -d -s VCamProducer VCamSplit VirtualCameraSession > "$dir/$run/logcat.txt"
echo "CASE_DONE $run"
