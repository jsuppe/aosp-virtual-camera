# Raspberry Pi5 / Android17 virtual-camera integration

## Test app source

[Complete Pi producer and split-camera app](apps/VCamProducer) includes its Soong build, manifest, launcher, producer, direct mirror, vsync clock and Camera2 split activity. These are the target-specific sources, not the older Android13 producer in the root platform directory.

Important entry points:

- [VCamProducerService.java](apps/VCamProducer/src/com/example/vcamproducer/VCamProducerService.java): mode advertisement, negotiated render cadence, GPU-owned quad geometry, clock texture and HAL queue/mirror ordering.
- [SplitDemoActivity.java](apps/VCamProducer/src/com/example/vcamproducer/SplitDemoActivity.java): actual Camera2 device/session, two TextureViews and freeze/resume.
- [DirectMirror.java](apps/VCamProducer/src/com/example/vcamproducer/DirectMirror.java): separate left EGL window using the same source uniforms/text texture.
- [VsyncFrameClock.java](apps/VCamProducer/src/com/example/vcamproducer/VsyncFrameClock.java): latest-only Choreographer mailbox. EGL stays on one graphics owner.

## Selectable60/120fps tests

The default640x480 RGBA mode advertises up to120fps. The normal split viewer requests `[60,60]`; the launcher also offers a **120 fps split-screen test**, equivalent to starting `SplitDemoActivity` with `--ei fps 120`. That test requests `[120,120]` and asks the window and output surfaces for120Hz. The producer uses the negotiated stream rate, not a test-only hardcoded override. The720p and4K default modes retain30fps;1080p now also has an experimental120fps ceiling. The actual display/app vsync cadence limits generated frames; a120fps request on a60Hz clock cannot deliver120fps. Declared capabilities alone do not establish sustained fresh-frame delivery.

The clock is a nominal one-second loop with60 or120 frames according to the negotiated rate. At120fps frame01≈0.008 and frame120=1.000, then repeat. It is frame-counter time, NOT a live monotonic stopwatch. Missed render deadlines can make a cycle last longer than one wall-clock second.

### 1080p split test

The launcher also offers **1080p /120fps split-screen test**, equivalent to
`SplitDemoActivity --ei fps 120 --ei width 1920`.
Both the producer-direct buffer and the real Camera2 output use1920×1080,
with16:9 layout. The1080p mode now advertises up to120 for this experiment;
VGA60 remains the default launch and720p/4K default modes remain30fps.
Use `--ei fps 60 --ei width 1920` for an explicit1080p60 request.
The split toolbar provides **Resolution** (VGA/1080p) and **Frame rate**
(30/60/120fps target) menus. Changing a setting reopens the preview; the
registered producer remains alive. Settings survive activity recreation.
The producer explicitly retains60fps endpoint modes alongside120fps modes;
raising only the maximum would otherwise remove `[60,60]` from the advertised
range list. The viewer validates advertised rates before submitting requests
and does not automatically reopen forever after a camera error.
Negotiation is not performance qualification: the first1080p120 physical run
did not sustain120 submissions or camera results/s. See the qualification record.
Each pane is scaled to fit half the monitor;1080p refers to its source/output
buffer, not1920 display pixels across each pane on a1920-wide monitor.

## What matching clocks mean

### Interactive live-path check

Tap the LEFT image while live to cycle the source background red → green → blue.
No tap numbers or tap instructions are shown in the UI. The activity only
publishes an immutable change request; it does not
recolor either TextureView. The EGL owner samples that request once per frame,
queues the changed pixels to the HAL, then draws the direct mirror with identical
uniforms. RIGHT remains exclusively a Camera2 preview. Freeze ignores taps.

This demonstrates causal, live propagation rather than prerecorded animation.
Rapid taps can coalesce before the next render deadline. `TAP_REQUEST` and
`TAP_ENQUEUED` logs measure click-handler to first changed-frame swap return,
NOT physical input-to-photon or Camera2/display latency. For that measurement,
record the click indication and both panes with a high-speed camera, or match
source markers to timestamped Camera2 images with explicitly defined endpoints.

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
