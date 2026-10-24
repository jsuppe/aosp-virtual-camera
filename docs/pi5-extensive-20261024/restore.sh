#!/system/bin/sh
set -eu
am force-stop com.example.vcame2e
am force-stop com.example.vcamproducer
su 0 setprop vendor.vcam.profile 0
su 0 setprop vendor.vcam.profile.clear false
su 0 setprop vendor.vcam.profile.deadline 1
su 0 sh /data/local/tmp/vcam-investigation/overlay.sh restore
am start -W -f 0x10008000 -n com.example.vcamproducer/.SplitDemoActivity --ei fps 60 --ei width 1920 --ez surface_preview true --ez mirror true --ez clock true
echo VCAM_INVESTIGATION_RESTORED
