#!/system/bin/sh
set -eu
dir=/data/local/tmp/vcam-investigation
q=$dir/qual
mkdir -p "$q"
app=com.example.vcame2e
producer=com.example.vcamproducer
trap 'sh "$dir/restore.sh"' EXIT
snapshot() {
 cp /data/user/0/$app/files/status.json "$q/$1.json"
 grep -q '"failure":""' "$q/$1.json"
}
start_case() {
 am force-stop "$app"
 am force-stop "$producer"
 sleep 1
 setprop vendor.vcam.profile "$run"
 setprop vendor.vcam.profile.clear "$clear"
 setprop vendor.vcam.profile.deadline 1
 am start-foreground-service -n "$producer/.VCamProducerService" --ez e2e true --ei run "$run" --ei producer 1 --ez paused false --ez mirror false --ez clock false --ei profile_run "$run"
 sleep 2
 am start -W -n "$app/.MainActivity" --es only_camera 100 --ez rgba true --ei run100 "$run" --ei width100 "$width" --ei height100 "$height" --ei fps100 "$fps"
}
for run in 811 812 813 814 815 816; do
 clear=true; width=1920; height=1080; fps=60
 case "$run" in 811|813) clear=false;; esac
 case "$run" in 811|812|816) width=640; height=480;; esac
 case "$run" in 815|816) fps=120;; esac
 echo "BEGIN_QUAL $run clear=$clear ${width}x${height}@$fps"
 start_case
 sleep 37
 snapshot "$run-status"
 am start -W -n "$app/.MainActivity" --ez export true
 sleep 2
 cp /data/user/0/$app/files/images.csv "$q/$run-images.csv"
 cp /data/user/0/$app/files/results.csv "$q/$run-results.csv"
 dumpsys media.camera > "$q/$run-camera.txt"
 echo "END_QUAL $run"
done
# Lifecycle positive controls, including stable no-image stall windows.
run=818; clear=true; width=640; height=480; fps=60
start_case
sleep 5
snapshot lifecycle-live
am start-foreground-service -n "$producer/.VCamProducerService" --ez e2e true --ei run "$run" --ez paused true
sleep 3
snapshot lifecycle-stall-a
sleep 3
snapshot lifecycle-stall-b
am start-foreground-service -n "$producer/.VCamProducerService" --ez e2e true --ei run "$run" --ez paused false
sleep 5
snapshot lifecycle-resumed
am start -W -n "$app/.MainActivity" --ez reopen100 true
sleep 5
snapshot lifecycle-reopened
am start -W -n "$app/.MainActivity" --ei width100 1920 --ei height100 1080
sleep 5
snapshot lifecycle-1080
am start -W -n "$app/.MainActivity" --ei fps100 120
sleep 5
snapshot lifecycle-120
am start -W -n "$app/.MainActivity" --ei fps100 60
sleep 5
snapshot lifecycle-60
am force-stop "$producer"
sleep 3
snapshot lifecycle-producer-dead
am start -W -n "$app/.MainActivity" --ei run100 819
am start-foreground-service -n "$producer/.VCamProducerService" --ez e2e true --ei run 819 --ez paused false
sleep 5
snapshot lifecycle-producer-restarted
# Deliberately wrong expected identity: this MUST fail, not be waived.
am force-stop "$app"
am start -W -n "$app/.MainActivity" --es only_camera 100 --ez rgba true --ei run100 99999 --ei width100 640 --ei height100 480 --ei fps100 60
sleep 3
cp /data/user/0/$app/files/status.json "$q/wrong-identity-negative.json"
grep -q 'wrong producer' "$q/wrong-identity-negative.json"
logcat -d -v threadtime -s VCamE2E VirtualCameraSession AndroidRuntime > "$q/log.txt"
echo VCAM_QUALIFICATION_DONE
