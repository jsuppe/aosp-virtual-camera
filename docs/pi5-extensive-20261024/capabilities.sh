#!/system/bin/sh
set -u
dir=/data/local/tmp/vcam-investigation
LD_LIBRARY_PATH=/apex/com.android.hardware.egl.mesa/lib64:/system/lib64 "$dir/modetest" -M vc4 -c -p > "$dir/kms-capabilities.txt" 2>&1
simpleperf list > "$dir/perf-events.txt" 2>&1
perfetto --query > "$dir/perfetto-sources.txt" 2>&1
cat /sys/kernel/debug/dri/0/v3d_ident > "$dir/v3d-ident.txt" 2>&1
for f in /sys/devices/system/cpu/cpufreq/policy0/scaling_governor /sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq; do echo "$f"; cat "$f"; done > "$dir/cpu-policy.txt"
echo VCAM_CAPABILITIES_DONE
