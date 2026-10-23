# Raspberry Pi5 / Android17 virtual-camera integration

## Test app source

[Complete Pi producer and split-camera app](apps/VCamProducer) includes its Soong build, manifest, launcher, producer, direct mirror, vsync clock and Camera2 split activity. These are the target-specific sources, not the older Android13 producer in the root platform directory.

Important entry points:

- [VCamProducerService.java](apps/VCamProducer/src/com/example/vcamproducer/VCamProducerService.java): mode advertisement, negotiated render cadence, GPU-owned quad geometry, clock texture and HAL queue/mirror ordering.
- [SplitDemoActivity.java](apps/VCamProducer/src/com/example/vcamproducer/SplitDemoActivity.java): actual Camera2 device/session, two TextureViews and freeze/resume.
- [DirectMirror.java](apps/VCamProducer/src/com/example/vcamproducer/DirectMirror.java): separate left EGL window using the same source uniforms/text texture.
- [VsyncFrameClock.java](apps/VCamProducer/src/com/example/vcamproducer/VsyncFrameClock.java): latest-only Choreographer mailbox. EGL stays on one graphics owner.

## Current60fps behavior

The default640x480 RGBA mode advertises up to60fps and the split viewer explicitly requests `[60,60]`. The producer uses the negotiated stream rate, not a test-only hardcoded override. Larger default modes retain30fps. A60Hz vsync clock limits the current design to approximately60 generated frames/s; declared capabilities alone do not establish sustained delivery.

The clock is a60-frame nominal one-second loop: frame01≈0.017, frame60=1.000, then repeat. It is frame-counter time, NOT a live monotonic stopwatch. Missed render deadlines can make a cycle last longer than one wall-clock second.

## What matching clocks mean

The right image really passes through Camera2. It is not a second independently drawn clock widget. But the left mirror is drawn AFTER the source frame is queued to the HAL and has its own presentation buffering. Matching clock values in a photograph mean matching step values in that exposure, not zero pipeline latency or a strict sub16.7ms bound. The one-second wrap can hide integer-second offsets.

Freeze captures one composited-window snapshot of both panes while producer/Camera2 continue. Resume live removes that overlay. For precise latency, use a nonwrapping source frame ID plus real monotonic timestamps, decode/match IDs from Camera2 images, measure well-defined endpoints and validate with a known right-only delay off the shared UI/render thread. Callback timing is not glass/presentation latency.

## Matched build

Base: AOSP `android-17.0.0_r1` plus Raspberry Vanilla board manifest commit `a155dda7f0db82d9b8076c1f39f41db4167fcb73`. Full pinned platform/board project revisions are in [source-manifest-pinned.xml](source-manifest-pinned.xml). Prepare a separate checkout matching that manifest; this helper does not download the Android source tree.

The app requires platform APIs, platform signing and the integrated broker/JNI/HAL; it is not standalone Gradle or APEX-only installation onto stock Android. The root repository source is the shared input; `integrate.py` overlays the full Pi app and emits the Android17-specific source/policy/build changes. It refuses nested source/target checkouts.

```bash
# After preparing the matched Android source tree and installing AOSP host deps:
./targets/rpi5-a17/build-image.sh /absolute/path/to/aosp /absolute/path/to/evidence
```

The build needs `mkfs.fat` and `mcopy` in PATH (or `VCAM_HOST_TOOLS_PATH`). `OUT_DIR` defaults to the absolute AOSP checkout's `out`; keep it consistent. Integration runs twice and compares source inventories; unchanged files are not rewritten. The build checks service-label consistency and APEX-only VINTF placement. Package images with `package-image.py --help`; the disk layout is matched to this specific board manifest, not arbitrary Pi images.

For app-only iteration after integration:

```bash
cd /absolute/path/to/aosp
source build/envsetup.sh
export OUT_DIR=/absolute/path/to/aosp/out
export SOONG_NINJA=ninja
lunch aosp_rpi5-cp2a-userdebug
m -j8 VCamProducer VCamProducer2
```

App installation changes only the app package; full factory-image rebuild/flash is a separate action. Existing R2 image archives are earlier snapshots and do not automatically include this60fps APK.

## Runtime evidence and boundaries

[Qualification summary](qualification.json) distinguishes actual checks from NOT_RUN claims. Before the VBO fix, decoded screen pixels recycled only17–19 while callback counters advanced. After the fix, a30fps diagnostic recording covered all30 step values with no backward candidates in98 samples. Screen recording adds software encoding load and can skip samples, so it is not a source-throughput or latency benchmark.

Upstream board baseline remains userdebug, unlocked/test-signed and SELinux permissive. A successful image build/demo is not Enforcing-mode or production-security qualification. The single-camera VGA RGBA result does not establish multicamera, YUV/JPEG,4K60, staged-OTA recovery or all-consumer compatibility. Rapid service stop/start slot-ID handoff remains a separate known lifecycle finding.
