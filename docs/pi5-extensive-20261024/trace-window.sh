#!/system/bin/sh
set -eu
run="$1"
dir=/data/local/tmp/vcam-investigation
instance=/sys/kernel/tracing/instances/vcam_extensive
mkdir "$instance"
trap 'echo 0 > "$instance/tracing_on"; echo 0 > "$instance/events/enable"; rmdir "$instance"' EXIT
echo 0 > "$instance/tracing_on"
echo mono > "$instance/trace_clock"
echo 8192 > "$instance/buffer_size_kb"
for event in v3d dma_fence gpu_scheduler sched/sched_switch sched/sched_wakeup power/cpu_frequency drm; do
 echo 1 > "$instance/events/$event/enable"
done
echo 1 > "$instance/tracing_on"
sleep 5
echo 0 > "$instance/tracing_on"
cat "$instance"/per_cpu/cpu*/stats > "$dir/trace-$run-stats.txt"
cat "$instance/trace" > "$dir/trace-$run.txt"
