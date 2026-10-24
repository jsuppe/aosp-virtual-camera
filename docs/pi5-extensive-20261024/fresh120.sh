#!/system/bin/sh
set -eu
dir=/data/local/tmp/vcam-investigation
q=$dir/fresh120
app=com.example.vcame2e
producer=com.example.vcamproducer
mkdir -p "$q"
uid=$(cmd package list packages -U "$producer" | grep -F "package:$producer " | sed -n 's/.*uid:\([0-9]*\).*/\1/p')
[ "$uid" = 10078 ]
dumpsys SurfaceFlinger > "$q/before-sf.txt"
if grep -q '^[[:space:]]*Backdoor=' "$q/before-sf.txt"; then echo EXISTING_OVERRIDE_REFUSE;exit 2;fi
sh "$dir/overlay.sh" install
cleanup() {
 service call SurfaceFlinger 1039 i32 "$uid" f 0
 sh "$dir/restore.sh"
}
trap cleanup EXIT
for run in 891 892; do
 am force-stop "$app"
 am force-stop "$producer"
 if [ "$run" = 892 ]; then service call SurfaceFlinger 1039 i32 "$uid" f 120; fi
 setprop vendor.vcam.profile "$run"
 setprop vendor.vcam.profile.clear true
 am start-foreground-service -n "$producer/.VCamProducerService" --ez e2e true --ei run "$run" --ei producer 1 --ez paused false --ez mirror false --ez clock false --ei profile_run "$run"
 sleep 2
 am start -W -n "$app/.MainActivity" --es only_camera 100 --ez rgba true --ei run100 "$run" --ei width100 1920 --ei height100 1080 --ei fps100 120
 sleep 37
 cp /data/user/0/$app/files/status.json "$q/$run-status.json"
 grep -q '"failure":""' "$q/$run-status.json"
 am start -W -n "$app/.MainActivity" --ez export true
 sleep 2
 cp /data/user/0/$app/files/images.csv "$q/$run-images.csv"
 cp /data/user/0/$app/files/results.csv "$q/$run-results.csv"
 dumpsys SurfaceFlinger > "$q/$run-sf.txt"
 echo "END_FRESH120 $run"
done
echo VCAM_FRESH120_DONE
