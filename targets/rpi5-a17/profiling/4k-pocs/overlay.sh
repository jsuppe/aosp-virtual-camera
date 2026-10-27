#!/system/bin/sh
# Temporary profiling only. Does not install/update an APEX or change its image.
set -eu
[ "$(id -u)" = 0 ]
[ "$(getprop ro.product.device)" = rpi5 ]
[ "$(getprop ro.build.version.sdk)" = 37 ]
target=/apex/com.android.hardware.camera.provider.virtual/lib64/android.hardware.camera.provider-V1-virtual-impl.so
payload=/data/local/tmp/vcam-investigation/impl.so
original=c344bea5b6c76f5d4ecceb91b9bc7065e393388b35483de6270ad86cb02ce1b8
diagnostic=db2f71eaaa779f7db9c092bffda0da02c0c8e6bfd159791c705e070fbf435724
profile_sha() { sha256sum "$1" | cut -d ' ' -f1; }
case "${1:-}" in
 install)
  [ "$(profile_sha "$target")" = "$original" ]
  [ "$(profile_sha "$payload")" = "$diagnostic" ]
  chown root:root /data/local/tmp/vcam-investigation "$payload"
  chmod 0755 /data/local/tmp/vcam-investigation
  chmod 0444 "$payload"
  stop vendor.camera-provider-virtual
  if ! mount -o bind "$payload" "$target"; then start vendor.camera-provider-virtual; exit 2; fi
  if [ "$(profile_sha "$target")" != "$diagnostic" ]; then umount "$target"; start vendor.camera-provider-virtual; exit 3; fi
  start vendor.camera-provider-virtual
  echo VCAM_DIAGNOSTIC_OVERLAY_INSTALLED
  ;;
 restore)
  [ "$(profile_sha "$target")" = "$diagnostic" ]
  stop vendor.camera-provider-virtual
  umount "$target"
  [ "$(profile_sha "$target")" = "$original" ]
  start vendor.camera-provider-virtual
  echo VCAM_FACTORY_LIBRARY_RESTORED
  ;;
 *) exit 64;;
esac
