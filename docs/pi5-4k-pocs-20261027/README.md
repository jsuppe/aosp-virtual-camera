# Pi5 UHD: five performance POCs — 2026-10-27

## Outcome

The old full split demo delivered **4.58–5.52 successful Camera2 results/s**.
The best tested combination—HAL clear + left SurfaceView + preview-sized reference,
30fps requested—delivered **27.02–28.62 results/s** in two fixed60-second windows.
The app-only pair (SurfaceView + preview-sized reference, no HAL clear) delivered
**25.03 results/s**. That pair is now the normal UHD demo default.

**These are Camera2 result rates, not photon/scanout rates.** Native successful
source sequences are unique in every measured run; independent pixel decoding
is reported separately below. There are18 full-split measurement windows
(18minutes measured, excluding warmup/traces) and two60-second pixel windows. This is not a claim of sustained4K60 or a hardware
maximum. Normal UHD requests remain capped at30. Clear stays experimental;
cached-clock and render-once stay default-off after negative isolated results.

## Five isolated strategies

1. **HAL clear-before-overwrite:** existing debug candidate avoids old destination
   tile loads. 8.67results/s; native source/destination fences unchanged.
2. **Left SurfaceView:** removes the left TextureView composition pass while
   retaining a UHD reference buffer for this isolated test. 10.00results/s.
3. **Preview-sized reference:** retain TextureView but render its visible960×540
   reference instead of another UHD buffer. 11.20results/s. The camera source
   and Camera2 buffers remain3840×2160; reference is not a native4K pixel comparator.
4. **Precomputed clock textures:** remove per-frame Canvas/upload work, retaining
   the same nominal clock appearance. **0 successful results/s**, despite25.78
   producer submissions/s.1547 source-fence timeouts +210 no-source outcomes.
5. **Render-once FBO:** one UHD scene draw followed by camera/reference texture
   copies. **0 successful results/s**, despite24.97 producer submissions/s.
   1498 source-fence timeouts +254 no-source outcomes. This is real scene reuse,
   but not zero-copy; the extra UHD pass did not help this baseline.

Zero-result runs had continuing requests, not absent consumers. Their source
GPU workload remained saturated. These are negative findings for these specific
POCs under this workload, not proof that every clock atlas or sharing design fails.

## Full comparison matrix

Each row:15s warmup, fixed60s device-monotonic measurement, followed by a separate
5s kernel GPU/scheduling trace. Same one-frame source deadline; no fence bypass,
kernel/HWC/allocator/governor change, screenrecord or build during rate windows.

|Run|Configuration|Producer/s|Camera2 success/s|Failed results/s|Callback age p95 ms|Gap p99 ms|
|---|---|---:|---:|---:|---:|---:|
|1001|Baseline|21.50|4.58|24.75|32.94|278.40|
|1002|HAL clear only|21.27|8.67|21.32|30.77|138.94|
|1003|Left SurfaceView only|20.00|10.00|20.00|17.97|100.90|
|1004|Preview-sized reference only|29.95|11.20|18.78|43.80|166.67|
|1005|Cached clock only|25.78|0.00|29.28|—|—|
|1006|Render-once only|24.97|0.00|29.20|—|—|
|1007|Baseline repeat|21.20|5.52|23.77|33.81|306.86|
|1010|Clear + left SurfaceView|20.00|10.00|20.00|16.83|103.11|
|1011|Clear + preview-sized reference|29.47|18.58|11.23|37.49|199.43|
|1012|Clear + SurfaceView + preview-sized @30|28.62|28.62|1.38|17.27|104.98|
|1013|Same three @60|30.00|6.27|50.72|17.46|466.98|
|1014|Same three @40|29.45|9.93|29.95|16.91|172.61|
|1015|Same three @45|29.03|14.50|30.50|17.76|134.00|
|1016|Same three @50|29.65|9.68|40.12|17.21|167.38|
|1017|Same three @30 repeat|28.33|27.02|2.98|17.72|134.27|
|1018|SurfaceView + preview-sized, no clear|28.45|25.03|4.97|20.73|170.73|
|1019|SurfaceView + preview-sized + cached clock @30, no clear|29.03|25.55|4.45|21.13|170.72|
|1020|Clear + SurfaceView + preview-sized + cached clock @60|51.68|6.98|50.28|19.19|296.77|

Age means producer BufferQueue timestamp → result callback entry; it excludes
GPU completion/display scanout and is not optical latency. Dashes for age/gap
in the two failed cases mean **no successful samples**, not zero latency.

The best initial combination delivered28.3–28.8 successes/s across its six10s
subwindows. Its27.02 repeat prevents claiming exact sustained30. Higher requested
rates40/45/50/60 all performed worse; the producer remained near30, with roughly
31–32ms elapsed in the reference-output stage. This stage includes GPU/queue/swap
backpressure, not just CPU drawing. The active monitor mode was1080p60 throughout.

## GPU and integrity evidence

Baseline render-engine busy was98.4%. Its trace attributed about65.5% of engine
time to producer draws and20.9% to Android RenderThread. SurfaceView-only reduced
RenderThread share to about0.1%. The three-change30fps run had69.6% render-engine
busy. Clear-only HAL hardware-start→IRQ median fell24.43→14.46ms; the combined
run had a9.98ms HAL median. These traces are separate post-window samples.

All retained per-CPU overrun/drop counters are zero. Independent GPU fence joins
have zero attribution mismatches; boundary jobs remain accounted for in JSON.
Native success counts have no repeated source sequence. Camera/native counts can
differ by one at window boundaries; these are distinct callback timestamps.
Boundary temperatures48.5–52.35°C, GPU957.4–993.6MHz and CPU2.0–2.4GHz do not
explain the large layout differences. These samples do not exclude every transient.

## Later interaction check and visual caveat

Cached clocks were revisited with the cheaper SurfaceView/reference layout:
25.55results/s without clear at30 requested, versus25.03 without caching. This
small single-run difference is not a demonstrated throughput gain. Adding clear
and requesting60 produced51.68 submissions/s but only6.98 successful results/s.
Thus faster production still does not imply faster camera delivery.

The previously documented clock-glyph artifact remains: the
[factory-HAL capture](final-4k.png) has missing static glyphs on the right, and
[the cached-clock capture](cached-clock.png) also has missing glyphs. Caching did
not qualify as a visual fix and is **not enabled by default**. The numeric/frame
clock is not a reliable optical latency meter. These images are screenshots of
scaled previews, not native4K pixel captures. Broad scene/clock correctness is
not established by the monochrome identity oracle below.

## Independent pixel checks and lifecycle

[Pixel summaries](pixels.json) use the existing RGBA identity decoder at3840×2160,
with producer/run/generation/sequence, RGB agreement and sampled alpha checks.
This separate workload disables the mirror/clock and overlays a marker pattern;
it is **not** the full split demo's workload.

- 30fps requested, clear enabled: **1800 distinct frames/60s =30.00 fresh fps**;
 zero native failures inside the window. Five capture failures occurred over the
 whole run including startup. Producer-enqueue→image-receipt median17.18ms,
 p9518.08ms, p9922.87ms, no missing timestamp joins or negative values.
- 60fps requested, clear enabled: **267 distinct frames/60s =4.45 fresh fps**,
 3226 native failures inside the window. Preserved, not pooled with the30fps run.

Latency joins image timestamp→Camera2 result frame number→native producer timestamp;
SENSOR_TIMESTAMP is the request pacing timestamp here, not the producer timestamp.
These endpoints exclude display scanout and are not optical latency.

[Lifecycle records](lifecycle.json): valid frames; deliberate wrong-run rejection;
paused producer with89 images unchanged while failed capture attempts continued
(first run), and88 unchanged in the corrected repeat; resume; consumer reopen;
UHD→VGA→UHD; producer disappearance and re-registration. The first restart harness
incorrectly reused run1053 after process death; the oracle rejected the reset
sequence/generation. That red result is retained. Repeating with new run1054
passed and delivered new frames. Neither result is hidden as a camera crash.

Normal app checks after restoring the factory HAL: live UHD source/camera,
960×540 reference, Freeze captured one compositor snapshot whose image-region
pixels stayed identical across two later screenshots, resume, and menu-driven
UHD→VGA→UHD all passed. Final state is live normal UHD with cached-clock and
render-once off. Temporary E2E APK removed; only the original bootreceiver/drm/wifi
trace instances remain. Profile0, clear=false, deadline1 restored. Factory HAL
hash verified `c344bea5b6c76f5d4ecceb91b9bc7065e393388b35483de6270ad86cb02ce1b8`.
No APEX/image update was installed, and the prior APK is retained locally.

## Reproduction and artifacts

- [Plan](PLAN.md), [case labels](cases.json), [performance](performance.json),
  [GPU attribution](gpu-attribution.json), [trace analysis](trace-analysis.json),
  [trace integrity](trace-integrity.json), [boundary system samples](system.json).
- [Scripts and experiment switches](../../targets/rpi5-a17/profiling/4k-pocs/README.md).
- Source: SplitDemoActivity.java (surface/layout), VCamProducerService.java
  (cached clock and diagnostic modes), SceneReuse.java (render-once FBO).
- Matched Pi AOSP17 CP2A.260605.016 userdebug/test-keys, SELinux permissive.
  Diagnostic HAL SHA256 `db2f71eaaa779f7db9c092bffda0da02c0c8e6bfd159791c705e070fbf435724`.
- Initial POC APK SHA256 `5e9c1a5676f55c2e7bcf79548dd97ca2e432b722d93ee9c1a603773c9ce12ba5`.
- Intermediate-rate APK SHA256 `29732bfe919ed139f714b5dd069ab82191682ed57319217208577565cbd9836e`.
- Final app APK SHA256 `e41fdc4a77c376d92dcb80c400f297da5bcdb6125f97fab8740faf30000bff9f`.

Initial cases1001–1013 use the first APK;1014–1018 add only diagnostic40/45/50
endpoints. Cases1019/1020 and pixel/lifecycle checks use the final APK.
Final APK promotes the app-only pair for normal UHD; diagnostic flags
retain the same explicitly selected behavior. All builds passed. Existing host
capability, fence ownership/fault and frame-identity positive/negative tests passed.

The first collection stopped because A17 am stopservice returned1 after
“Service stopped”; all trace files survived and case1001 was recovered. The
harness now checks the success text. Loom pre-edit briefing remains unavailable
because chromadb is missing. No unrelated dependency repair was performed.

## Qualification boundary

No4K recording/JPEG/dual-camera soak, arbitrary-app, full-color/transparent-alpha
coverage or Enforcing/production deployment qualification. No optical latency or
independently identified scanout FPS measurement. The v0.1.0 release is unchanged.
Original dirty A13 source is untouched. Failed POCs remain opt-in for reproduction.

## Retained raw archive

Local `exports/VCAM-4K-PERF-20261027-evidence.tar.gz` retains raw traces, CSVs,
failed harness attempts, source snapshots, diagnostic payload and final/previous
app APKs. SHA-256:
`331db821196a91b43e570be69349ce8a023c9155279673614600dd2ccf853897`.
Raw device/process inventories remain local; published JSON contains the measured
summaries and explicit failure records.
