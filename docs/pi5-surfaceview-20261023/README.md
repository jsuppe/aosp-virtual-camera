# Pi 5: SurfaceView camera-preview comparison

## Delivered change

The split demo now defaults to a SurfaceView for its right Camera2 preview. A **Preview: Surface / Texture** button provides runtime A/B selection. Resolution, frame-rate and tap-to-change-background controls remain. The left producer preview remains a TextureView to isolate this experiment.

The camera borrows the SurfaceHolder-owned surface (does not release it), waits for the negotiated fixed-size RGBA buffer, and preserves lifecycle/epoch guards. Freeze captures both panes in one compositor snapshot, not two asynchronously copied images. This Android 17 platform-signed test app requests signature READ_FRAME_BUFFER for that user-triggered capture; default secure/protected-content redaction is retained. Texture mode retains window PixelCopy. This is not a portable unprivileged-app capture API.

## Matched experiment

All four cases: 1920×1080, 120 fps target, same APK and unchanged factory HAL. Each throughput measurement is 60 seconds after five seconds of warm-up. A separate five-second kernel GPU trace follows each run; trace timings are not from the throughput window. No screen recording or builds during measured windows. Single ordered runs, not repeated randomized trials.

| Case | Camera preview | Producer mirror + clock | Producer submissions/s | Successful Camera2 results/s | Median timestamp→callback ms | p95 ms |
|---|---|---|---:|---:|---:|---:|
| 501 | TextureView | off | 120.00 | 87.12 | 12.351 | 16.933 |
| 502 | SurfaceView | off | 120.00 | 96.07 | 7.023 | 10.124 |
| 503 | TextureView | on | 76.35 | 7.17 | 10.836 | 18.833 |
| 504 | SurfaceView | on | 72.03 | 13.55 | 10.124 | 11.309 |

These are submissions and successful callbacks, **not independently decoded fresh-pixel FPS or photon latency**. Failed results are retained in analysis.json. Successful-result latency excludes failed requests. No missing or negative producer timestamps in successful results.

## What GPU tracing explains

Render-engine occupancy remains approximately 98–99%. Camera-only SurfaceView reduces app RenderThread GPU jobs from 378 to 18 in the separate five-second trace; its engine share falls from 37.57% to 1.55%. SurfaceFlinger reports the camera SurfaceView as DEVICE-composed on the active 1920×1080 120 Hz display.

However, median HAL output GPU service rises from **2.504 to 5.042 ms** (camera-only), and from **3.001 to 5.534 ms** in the full demo. Thus eliminating the UI texture redraw does not make this path zero-copy or sustain 120 successful frames/s. The full demo still incurs the left TextureView redraw, producer mirror rendering and clock work.

SurfaceView output allocations are RGBA8888, usage 0xb00, DRM modifier 0 (linear). The old TextureView camera destination allocation is not independently identified in the SurfaceFlinger dump. A destination layout/usage effect is a hypothesis, **not a proven tiled-to-linear transition**. Next targeted experiment: inspect both camera destination allocations and trace the HAL pass with a non-displaying consumer, then vary resolution and output layout without changing the producer.

GPU job attribution joins scheduler fences and V3D render completion; an independent fence-origin cross-check found zero mismatches (1,401 / 1,165 / 1,195 / 1,150 checks). Boundary jobs are retained as unmatched counts. No reported trace overruns. See aggregate JSON and analyzers for CPU/GPU timing distributions and sample counts.

## Validation and provenance

- Both producer APK targets built successfully; 48 Python tests passed; git diff --check passed.
- Live SurfaceView camera pixels at 1080p60 verified. A frozen comparison crop was pixel-identical across two screenshots two seconds apart, with both panes present.
- Device UI checks exercised Surface→Texture→Surface, home/resume, tap color propagation, VGA120→60→30 and 1080p30→60; live results resumed without a new app fatal exception in the collected log. Final configuration is SurfaceView, full split demo, 1080p60.
- Existing tap diagnostic re-emits an old tap timestamp after renderer recreation; those stale TAP_ENQUEUED values are not latency measurements and were not used here.
- Platform remains development userdebug/permissive SELinux; this is not security qualification.
- Factory HAL remained running, no native overlay or APEX update in this experiment.
- APK SHA-256: `0e1dfe3189416e11296c696e118ba193e2febe146a3af38fdb3b6f5364c6b27b`.
- HAL implementation SHA-256: `c344bea5b6c76f5d4ecceb91b9bc7065e393388b35483de6270ad86cb02ce1b8`.
- Raw archive (local, not committed): `raw-results.tgz`, 12,925,487 bytes, SHA-256 `887230980348efd8ddc7f7e49db34d946e64b3d6a150fc6bcccb61194bfcd13d`.
- To reproduce aggregate analysis, safely extract that archive into `extracted/` alongside the supplied Python scripts. Raw logs stay local; this directory contains aggregate measurements only.

## Source

- [SplitDemoActivity](../../targets/rpi5-a17/apps/VCamProducer/src/com/example/vcamproducer/SplitDemoActivity.java)
- [Manifest](../../targets/rpi5-a17/apps/VCamProducer/AndroidManifest.xml)

The unchanged factory HAL still renders the producer texture into the Camera2 destination. A SurfaceView changes how that destination reaches composition; it does not remove the HAL draw. GPU service times above are hardware submission-to-completion intervals, not merely Java call duration or a buffer-queue operation.
