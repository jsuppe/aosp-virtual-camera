#!/system/bin/sh
# Separate pixel-decoding workload, NOT the full split scene.
set -eu
run=$1; fps=$2; rgba=$3; clear=$4
app=com.example.vcame2e
producer=com.example.vcamproducer
dir=/data/local/tmp/vcam-4k-pocs/pixel-$run
mkdir -p "$dir"
am force-stop "$app"
am force-stop "$producer"
setprop vendor.vcam.profile "$run"
setprop vendor.vcam.profile.clear "$clear"
setprop vendor.vcam.profile.deadline 1
am start-foreground-service -n "$producer/.VCamProducerService" --es profile 4k-perf --ei profile_run "$run" --ez e2e true --ei run "$run" --ei producer 1 --ez mirror false --ez clock false
sleep 2
am start -W -n "$app/.MainActivity" --es only_camera 100 --ez rgba "$rgba" --ei run100 "$run" --ei width100 3840 --ei height100 2160 --ei fps100 "$fps"
sleep 82
am start -W -n "$app/.MainActivity" --ez export true
sleep 2
cp /data/user/0/$app/files/status.json "$dir/status.json"
cp /data/user/0/$app/files/images.csv "$dir/images.csv"
cp /data/user/0/$app/files/results.csv "$dir/results.csv"
# Preserve native source identity / producer timestamp traces for joins.
am force-stop "$app"
sleep 2
stop_result=$(am stopservice -n "$producer/.VCamProducerService" 2>&1) || true
printf '%s\n' "$stop_result"
case "$stop_result" in *"Service stopped"*) ;; *) exit 2;; esac
sleep 2
cp /data/vendor/vcam-profile/hal-"$run"-*.csv "$dir/"
cp /data/user/0/$producer/files/vcam-profile/producer-"$run"-*.csv "$dir/"
echo "PIXEL_CASE_DONE $run"
