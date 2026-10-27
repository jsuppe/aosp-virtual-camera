# Pi5 UHD demo enablement — 2026-10-27

The shared HAL/default producer already advertise 3840×2160. This change adds
that resolution to the Pi split demo and launcher, with a 30-fps request ceiling.
No HAL/APEX/system update, display-mode change or new 4K60 claim is involved.

## Matched build and tests

- Base: Raspberry Pi5 AOSP17 CP2A.260605.016 userdebug/test-keys, existing
  SELinux-permissive reference image. Two 1080p HDMI outputs connected.
- Fresh feature worktree from `db7258d`; original dirty A13 checkout untouched.
- Only MainActivity.java and SplitDemoActivity.java changed in the matched A17
  build tree. App Java inputs compared against target sources.
- `OUT_DIR=/mnt/micron/aosp-a17-rpi5-vcam-20261022/out`,
  `lunch aosp_rpi5-cp2a-userdebug`, `m -j8 VCamProducer`: passed.
- `bash scripts/test-producer-capabilities-host.sh`: passed, including UHD30
  selection and rejection of UHD60 against a UHD30 descriptor.
- Updated platform-signed APK installed with `adb install -r`; previous APK saved.
- Launching width3840/fps120 logs `SURFACE_PREVIEW_READY 3840x2160` and
  `REQUESTED_FPS 30`: invalid UHD high-rate requests are normalized.
- UI menu switched UHD → 1080p → VGA → UHD; each reopened successfully.
  Final state is live UHD. Temporary acceptance-probe APK removed after testing.
- Camera service dump confirms RGBA output dimensions 3840×2160. The
  [screen capture](split.png) shows the resulting scaled split preview, not
  a native-resolution 4K monitor image or optical latency measurement.

APK SHA-256:
`69ae0b50665ab3003acd85ba807e5cf8c7364a12ac067974bd1aea4c883e03e5`.

## Independent pixel-delivery checks

Used the existing public-Camera2 VCamE2E consumer with producer identity markers,
run427, camera100, width3840, height2160, fps30. Producer mirror/clock disabled
for these consumer checks; no experimental HAL clear/deadline overlay applied.

- YUV420: **710 distinct decoded frames over 23.721 seconds**, 29.889 interval
  fps. Later status had 917 valid images, zero capture failures/device errors.
- RGBA repeat: **282 distinct decoded frames over 9.487 seconds**, 29.618
  interval fps; **3 capture failures**, zero device errors.
- Initial RGBA run: 323 decoded images, **8 capture failures**, zero device
  errors. Retained separately; the repeat does not erase this result.
- Wrong-run negative control: consumer expecting99999 rejected decoded run427.
  This demonstrates the identity check is active rather than only counting callbacks.

Raw sample CSVs, status snapshots, build/preview logs and `summary.json` accompany
this record. Interval fps = (samples−1)/(last receipt−first receipt).
The first RGBA CSV export/pull raced the asynchronous writer; no CSV result is
claimed for that attempt. Subsequent exports were collected after completion.
Loom's pre-edit check could not load its missing chromadb dependency; source
inspection and the existing capability tests were used directly.

## Limits

These are short functional samples, **not sustained/soak qualification**. Full
split rendering has additional producer-mirror/UI GPU work and does not inherit
these consumer rates. The initial split log advanced from30 to90 successful
results over12.167 seconds (about4.93 results/s); this is callback throughput,
not a distinct-pixel or display FPS measurement. No 4K60, 4K JPEG/encoder/recording, optical latency,
Enforcing compatibility or arbitrary application qualification is claimed.
The existing v0.1.0 release is unchanged; this is an app-source update.

See [Pi build/use instructions](../../targets/rpi5-a17/README.md).
