#!/system/bin/sh
set -eu
am start -W -f 0x10008000 -n com.example.vcamproducer/.MainActivity
su 0 setprop vendor.vcam.profile 0
su 0 setprop vendor.vcam.profile.deadline 1
su 0 stop vendor.camera-provider-virtual
sleep 1
su 0 sh /data/local/tmp/vcam-deadline/overlay.sh restore
[ "$(sha256sum /data/local/tmp/vcam-profile-restored.apk | cut -d ' ' -f1)" = 75fdea8f223bdd0a554050a61c32f11e6ddc822cea4e420cc3bb12219e219a9d ]
pm install -r /data/local/tmp/vcam-profile-restored.apk
am force-stop com.example.vcamproducer
am start -W -f 0x10008000 -n com.example.vcamproducer/.SplitDemoActivity --ei fps 60 --ei width 1920
echo VCAM_DEADLINE_RESTORED
