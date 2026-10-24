#!/system/bin/sh
set -eu
[ "$(id -u)" = 0 ]
run="$1"
case "$run" in 401|402|403|404|405) ;; *) exit 64;; esac
dir=/data/local/tmp/vcam-deadline
instance=/sys/kernel/tracing/instances/vcam_deadline
mkdir "$instance"
trap 'echo 0 > "$instance/tracing_on"; rmdir "$instance"' EXIT
echo 0 > "$instance/tracing_on"
echo mono > "$instance/trace_clock"
echo 8192 > "$instance/buffer_size_kb"
for event in v3d dma_fence gpu_scheduler sched/sched_switch sched/sched_wakeup power/cpu_frequency; do
 echo 1 > "$instance/events/$event/enable"
done
echo 1 > "$instance/tracing_on"
sleep 5
echo 0 > "$instance/tracing_on"
cat "$instance"/per_cpu/cpu*/stats > "$dir/trace-$run-stats.txt"
cat "$instance/trace" > "$dir/trace-$run.txt"
echo 0 > "$instance/events/enable"
rmdir "$instance"
trap - EXIT
