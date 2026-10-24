#!/system/bin/sh
set -eu
dir=/data/local/tmp/vcam-investigation
app=com.example.vcamproducer
instance=/sys/kernel/tracing/instances/vcam_present
cleanup() {
 if [ -d "$instance" ]; then echo 0 > "$instance/tracing_on"; echo 0 > "$instance/events/enable"; rmdir "$instance"; fi
 sh "$dir/restore.sh"
}
sh "$dir/overlay.sh" install
trap cleanup EXIT
for run in 881 882 883; do
 surface=true; mirror=false; clock=false
 case "$run" in 882) mirror=true;clock=true;; 883) surface=false;; esac
 am force-stop "$app"
 setprop vendor.vcam.profile "$run"
 setprop vendor.vcam.profile.clear true
 am start -W -f 0x10008000 -n "$app/.SplitDemoActivity" --ei fps 120 --ei width 1920 --ei profile_run "$run" --ez surface_preview "$surface" --ez mirror "$mirror" --ez clock "$clock"
 sleep 5
 mkdir "$instance"
 # Only this disposable instance: never change parent/global/other instance permissions.
 chown -R 9999:9999 "$instance"
 before=$(pidof traced_probes)
 perfetto --txt -c "$dir/presentation-original.cfg" -o "$dir/present-$run.pftrace" > "$dir/present-$run.log" 2>&1
 after=$(pidof traced_probes)
 echo "PERFETTO_PIDS $run $before $after"
 [ "$before" = "$after" ]
 echo 0 > "$instance/tracing_on"
 echo 0 > "$instance/events/enable"
 rmdir "$instance"
 dumpsys SurfaceFlinger > "$dir/sf-$run.txt"
 echo "END_PRESENT $run"
done
echo VCAM_PRESENTATION_DONE
