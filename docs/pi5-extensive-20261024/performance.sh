#!/system/bin/sh
set -eu
dir=/data/local/tmp/vcam-investigation
app=com.example.vcamproducer
printk_saved=$(cat /proc/sys/kernel/printk)
ignore_saved=$(cat /sys/module/printk/parameters/ignore_loglevel)
recpid=
cleanup() {
 [ -z "$recpid" ] || kill "$recpid" 2>/dev/null || true
 echo "$printk_saved" > /proc/sys/kernel/printk
 echo "$ignore_saved" > /sys/module/printk/parameters/ignore_loglevel
 sh "$dir/restore.sh"
}
grep -q VCAM_QUALIFICATION_DONE "$dir/qualify.log"
grep -q VCAM_INVESTIGATION_RESTORED "$dir/qualify.log"
sh "$dir/overlay.sh" install
trap cleanup EXIT
snapshot() {
 echo SNAPSHOT "$1"; cat /proc/uptime; cat /proc/stat
 cat /sys/class/thermal/thermal_zone0/temp
 cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq
 cat /sys/kernel/debug/dri/0/measure_clock
 cat /sys/kernel/debug/dri/0/bo_stats
 cat /proc/sys/kernel/printk; cat /sys/module/printk/parameters/ignore_loglevel
}
for run in 831 832 833 834 835 836 837 838 839 840 841 842; do
 surface=true; fps=120; width=1920; clear=true; mirror=false; clock=false; minimal=false
 case "$run" in 831) clear=false;; 833) mirror=true;; 834) clock=true;; 835|836|837) mirror=true; clock=true;; 838) surface=false;; 842) minimal=true;; esac
 case "$run" in 836) fps=60;; 837) width=640;; esac
 am force-stop "$app"
 sleep 1
 setprop vendor.vcam.profile "$run"
 setprop vendor.vcam.profile.clear "$clear"
 setprop vendor.vcam.profile.deadline 1
 echo "BEGIN_PERF $run surface=$surface width=$width fps=$fps clear=$clear mirror=$mirror clock=$clock minimal=$minimal"
 if [ "$run" = 840 ]; then echo N > /sys/module/printk/parameters/ignore_loglevel; echo '1 4 1 7' > /proc/sys/kernel/printk; fi
 am start -W -f 0x10008000 -n "$app/.SplitDemoActivity" --ei fps "$fps" --ei width "$width" --ei profile_run "$run" --ez surface_preview "$surface" --ez mirror "$mirror" --ez clock "$clock" --ez minimal_scene "$minimal"
 if [ "$run" = 839 ]; then
  screenrecord --time-limit 75 --size 1280x720 --bit-rate 4000000 "$dir/load-$run.mp4" > "$dir/record-$run.log" 2>&1 &
  recpid=$!
 fi
 snapshot before > "$dir/system-$run.txt"
 sleep 70
 snapshot after >> "$dir/system-$run.txt"
 pidof android.hardware.camera.provider-virtual-service > "$dir/halpid-$run.txt"
 sh "$dir/trace-window.sh" "$run"
 if [ -n "$recpid" ]; then wait "$recpid" || echo RECORD_FAILED; recpid=; fi
 dumpsys media.camera > "$dir/camera-$run.txt"
 dumpsys SurfaceFlinger > "$dir/sf-$run.txt"
 logcat -d -v threadtime -s VirtualCameraSession VCamGpuCompositor > "$dir/hal-$run.txt"
 case "$run" in 832|835|838)
  mkdir /sys/kernel/tracing/instances/vcam_present
  perfetto --txt -c "$dir/presentation.cfg" -o "$dir/present-$run.pftrace" > "$dir/present-$run.log" 2>&1 || echo PERFETTO_FAILED
  rmdir /sys/kernel/tracing/instances/vcam_present || echo PERFETTO_INSTANCE_REMAINS
  ;;
 esac
 am start -W -f 0x10008000 -n "$app/.MainActivity"
 sleep 2
 if [ "$run" = 840 ]; then echo "$printk_saved" > /proc/sys/kernel/printk; echo "$ignore_saved" > /sys/module/printk/parameters/ignore_loglevel; fi
 echo "END_PERF $run"
done
echo VCAM_EXTENSIVE_PERFORMANCE_DONE
