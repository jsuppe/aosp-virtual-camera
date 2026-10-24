# Pi 5 1080p120: extensive cross-subsystem investigation

## Executive finding

**This is not evidence that the Pi 5 cannot transport 1080p120. The present full split-screen demo performs too much GPU work, and its one-frame source-readiness deadline turns that pressure into failed camera requests.**

With the temporary clear-before-overwrite candidate and mirror disabled, the normal scene sustained **120 successful Camera2 results/s for 60 seconds**, repeated with ordinary and quiet console settings. Clock-only achieved 119.98/s. The full 1080p120 split demo achieved 12.92/s, while the same full demo achieved 60/s at 1080p60 and 120/s atVGA120.

Those rates are metadata callbacks, not by themselves independent pixel or photon proof. Pixel identity, native source sequence and presentation evidence are reported separately below. The candidate remains default-off; this investigation does not silently promote it into the factory HAL.

## Experiment and integrity

- 12 long cases: each has a fixed 60-second rate window after 5 seconds warm-up, followed by a separate 5-second V3D/scheduler/DRM trace. 720 seconds of rate windows in total.
- Same delivered producer APK throughout. Debug native library adds bounded CPU-substage timings; it does not change fences or the source deadline. Clear is the existing debug/profile-only experiment.
- All native trace overrun/drop counters are zero. Independent GPU command-list/fence attribution cross-checks have zero mismatches. Boundary jobs are accounted for separately in JSON.
- No missing producer timestamps or negative successful callback latencies. Successful native source sequences are unique in every measured case; this is stronger than callback counting but is still not pixel decoding.
- Three corrected, probe-stable 10-second presentation captures followed, with stable traced_probes PID and private-instance cleanup. Initial failed Perfetto attempts are preserved and excluded from presentation comparisons.
- [Methods, caveats and preserved failures](METHOD.md); [source paths and candidate changes](SOURCE-MAP.md).

## Long-window results

All cases are 1080p120 unless named otherwise. Clear is ON except 831. Mirror and clock are OFF unless named. GPU busy is from the subsequent trace, not the same 60-second window.

|Run|Workload|Producer submissions/s|Successful Camera2 results/s|GPU render-engine busy|
|---|---|---:|---:|---:|
|831|Surface, clear OFF, mirror/clock off|120.00|97.92|98.9%|
|832|Surface, clear ON, mirror/clock off|120.00|120.00|75.1%|
|833|Surface, mirror only|84.35|0.00|98.2%|
|834|Surface, clock only|120.00|119.98|83.9%|
|835|Surface, full mirror + clock|74.37|12.92|97.9%|
|836|Surface, full, 1080p60|60.00|60.00|89.5%|
|837|Surface, full, VGA120|120.00|120.00|69.2%|
|838|Texture, mirror/clock off|120.00|97.58|98.2%|
|839|Surface, screenrecord load|115.45|53.45|88.2%|
|840|Surface, quiet kernel console|120.00|120.00|79.2%|
|841|Surface, ordinary-console repeat|120.00|120.00|77.9%|
|842|Surface, minimal shader|120.00|120.00|66.6%|

The repeated camera-only cases 832/840/841 each delivered 7200 successful results in 60 seconds, with 120/s in every 10-second subwindow. Full split 835 varied from 11.3 to 14.5/s across those subwindows. Recording 839 remained near 53/s, rather than merely suffering one startup hiccup.

**Important red result:** mirror-only 833 produced 84.35 frames/s but delivered **zero** successful camera results during the full minute. Its native failures were 5061 source-fence timeouts and 1571 no-fresh-source outcomes. Adding the clock did not monotonically worsen this case: it changed where the producer blocked and yielded 12.92/s. This is scheduling/deadline coupling, not a simple independent “clock costs X fps” model. The failed case is retained, not averaged away.

## 1. HAL / GLES: real pixel work and an avoidable destination load

The HAL imports the source and destination, creates an output renderbuffer, samples the source across a full-screen quad, and returns a completion fence. It is not merely queueing an already-compatible framebuffer.

- Clear OFF831: HAL GPU job median **5.048 ms**, render engine 98.9% busy,97.92 successful results/s.
- Clear ON832: HAL GPU job median **2.408 ms**, render engine 75.1% busy,120/s.
- Repeated clear-ON camera-only controls 840/841: **3.078/3.003 ms**,79.2/77.9% busy, both 120/s.

Thus the improvement repeats, but a single fixed “2.4 ms” cost would be misleading: pipeline phase and concurrent work change individual job timings. Previous short experiments also showed approximately 5→3 ms. Mesa source marks old color tiles for load unless clear/invalidate removes that requirement, matching the observed direction. GPU memory-byte counters were not collected, so this is not a measured DRAM-bandwidth saturation claim.

## 2. Producer, mirror and Android RenderThread: dominant full-demo work

In full split 835, the 5-second trace attributes approximately:

- **54.1%** of render-engine time to producer jobs: 745 jobs, about two draws per produced frame; median job service 3.633 ms.
- **39.6%** to Android RenderThread: 373 jobs, median 5.314 ms.
- **4.1%** to the HAL:64 jobs, median 3.163 ms. The low HAL share reflects failed requests, not a cheap camera path.

The left pane is still a TextureView. The producer renders the scene into the camera input and draws it again into the reference window; Android then composites the reference texture into the app window. Moving the right pane to SurfaceView removed one composition dependency, not all of them.

The clock redraws a 512×256 Canvas bitmap and uploads it each frame. Its CPU-*elapsed* interval is 1.485 ms median in clock-only 834, but 11.594 ms in saturated full 835. That larger value includes graphics backpressure; it is not proof that text drawing alone consumes 11.6 ms of CPU. Mirror-only 833 instead blocks mainly in swap, median 8.301 ms. Waits move between API calls as pipeline conditions change.

Minimal shader 842 reduces producer GPU median from 3.311 ms in the matched ordinary repeat 841 to 2.227 ms, and total render-engine busy from 77.9% to 66.6%. Both already sustain 120 result callbacks/s. The scene itself has measurable cost, but the duplicated mirror and app composition are the larger full-demo targets.

**First implementation target after the clear candidate:** make the left preview a SurfaceView and measure again; then implement genuine render-once reuse with correct buffer/fence ownership. Do not revive the rejected offscreen prototype that still rendered the mirror independently.

## 3. Gralloc / minigbm / modifier negotiation

Runtime allocations still differ:

- SurfaceView: usage 0xb 00, linear RGBA output.
- TextureView: usage 0x 300, UIF-tiled output.

The HW_COMPOSER/scanout path uses the legacy GBM allocation API; matched Mesa forces that scanout allocation linear. Crucially, the actual KMS query reports 56 AB24 plane entries with **VC4_T_TILED and LINEAR**, not UIF. Matched V3D source advertises **UIF and LINEAR** for RGBA. Their shared layout here is **LINEAR**.

Therefore “enable modifier negotiation” is not, by itself, a demonstrated fix. Forcing UIF or stripping compositor usage could break scanout or introduce a hidden fallback. A genuine shared supported layout or a measured conversion strategy is required. No allocator changes were made.

## 4. Mesa / V3D / memory work

The RCL render engine is the saturated resource in the bad cases:98–99% busy; BCL/binning occupancy is only roughly 5–7%. The clear and simpler-shader experiments reduce actual hardware-start→IRQ intervals/occupancy, not merely Java submission time.

At 1080p120, one RGBA8 read or write is approximately 0.995GB/s of logical pixels. Several passes can multiply that workload. This arithmetic is not a bandwidth measurement. Available simpleperf PMU counters are CPU counters; Mesa exposes a separate per-context V3D PERFMON path. GPU cache/byte counters and a layout-only microbenchmark remain **NOT MEASURED**. No Mesa, firmware or kernel GPU-driver patch was installed.

## 5. Camera2 / BufferQueue / freshness

Normal Surface cases report 8 allocated output buffers, max HAL buffers 4, usually 0–1 currently dequeued; Texture reports 7 allocated. Capacity is not queue depth or a claim of eight frames of latency. The relay has a bounded BufferItemConsumer and a latest-frame HAL slot with configuration/sequence identity, not 30-frame batches.

In healthy 832, median destination-fence wait is 0.005 ms (p 990.010 ms), source-acquisition CPU phase 0.003 ms, and import/cache lookup 0.002 ms. Destination-buffer waiting is not the primary measured bottleneck. Source readiness is: full 835 has 3687 source-fence timeouts and 1903 no-source outcomes versus 775 successes. Successful output identities are unique; failed requests do not replay old frames to inflate rate.

The separate RGBA ImageReader validates changing marker identities in all RGB channels plus sampled alpha. Six initial mode/clear combinations have no decoded duplicates/regressions; stall/resume, reopen, mode changes, producer death/restart and deliberate wrong-run rejection passed. Initial 120-requested cases delivered about 60 distinct frames/s, so those cases alone do not qualify 120-pixel delivery. See the freshness follow-up below.

## 6. Fence waiting / deadline policy

Source availability and fence readiness share one requested frame period: about 8.33 ms at 120 fps. The current cancellable polling interval is 2 ms. In healthy 832, signal→observation median is 1.211 ms and p 991.958 ms. The ordinary repeat 841 has median 1.359 ms and p 993.344 ms, including other observation/scheduling work.

This is a meaningful secondary optimization target: use cancellation-aware event/fence notification and preserve generation invalidation and ownership. It does not explain away the roughly 98% GPU occupancy in the full demo. Increasing the deadline can trade latency for result count; it must not be described as faster rendering. Source/destination fences were never bypassed in these tests.

## 7. SurfaceFlinger / HWC / presentation

Post-window snapshots show DEVICE composition for the app window and the separate right camera SurfaceView. TextureView has only the app window at HWC; its texture composition happened inside the app. DEVICE composition therefore does **not** mean “no GPU work anywhere.”

Corrected presentation capture 881 (camera-only Surface) has 1178 display-frame records, all labeled On-time/None by FrameTimeline, and no GPU composition by SurfaceFlinger. Full split 882 has 839 display records; Texture 883 has 806. These are display records, not independently identified camera frames. Full split can present changes in the left pane while the camera output stalls.

Full-capture software display-completion interval p 95 is 8.654 ms for 881,16.987 ms for 882 and 25.002 ms for 883. The camera-only capture includes a 133.334 ms early gap and a 33.607 ms gap near 1.25s despite no FrameTimeline jank label. Do not turn “no jank label” into a guarantee of perfectly uniform fresh presentation. Initial tracing setup and incomplete per-layer identity limit interpretation. Captures 882/883 also contain 1/2 FrameTimeline parser errors; ftrace overrun/loss counters are separately zero. Perfetto service statistics also report 3/4/5 discarded chunks in 881/882/883; these counters and parser errors are retained in presentation.json. Capture completeness is not assumed.

No HWC patch or forced CLIENT/DEVICE composition was applied. The recording-time composition path is evidenced by extra RenderEngine jobs; the post-recording dump alone does not prove how every recorded frame was composed.

## 8. DRM/KMS / physical display

The active mode is 1920×1080 at 120 Hz; runtime plane capabilities and active mode were queried without modesetting. Corrected DRM trace median vblank period is approximately 8.333 ms. Logged sequence gaps are preserved; absence of a logged event is not proof that physical scanout stopped.

KMS is still a relevant compatibility/presentation boundary, but there is no evidence here that changing the display refresh mode is the primary fix. Optical input-to-photon latency, monitor processing and scanout-position effects remain **NOT MEASURED**. Equal photographed timestamps cannot establish zero latency.

## 9. Kernel scheduling / DVFS / thermal

Boundary GPU-clock samples are 960.1–990.9 MHz and temperature samples 49.05–51.8°C. CPU frequencies vary 2.1–2.4GHz at the boundaries. This does not exclude every transient throttle, but there is no observed clock-halving or thermal correlation explaining the GPU-pass difference.

Producer runnable→CPU p 99 is 0.136 ms in healthy 832 and 0.272 ms in full 835, versus multiple milliseconds of GPU service/queue delay. Under recording 839 it rises to 0.588 ms. Aggregate four-core CPU busy fraction is roughly 14–25% in ordinary cases and 42.4% with recording. CPU scheduling contributes to tails but is not the main full-demo bottleneck in these traces.

No governor, affinity, priority, thermal policy or kernel scheduling changes were made. This is a bounded soak across the matrix, not a day-long thermal qualification.

## 10. Serial logging / recording / diagnostics

- Quiet kernel console 840 and ordinary repeat 841 both sustain 120/s. Their GPU busy and CPU occupancy do not show a consistent quiet-console advantage. Both 5-second trace windows contain zero timestamp-matched kernel-console bytes. This does not prove heavy boot/error printing is free; it shows idle serial logging is not the principal bottleneck here.
- Screenrecord 839 reduces the working camera-only case to 53.45 successful results/s and producer 115.45/s. Successful callback gap p 99 rises to 67.0 ms, max 80.2 ms. CPU busy rises to 42.4%; the trace gains RenderEngine GPU jobs. The recording control is 1280×720 at 4Mbps, not an exact reproduction of scrcpy audio/network/encoding settings.
- Full split 835 has successful callback gap p 99253.5 ms and max 338.0 ms: those are real intermittent output stalls even though average producer motion may look active.
- Initial Perfetto private-instance permission failures caused probe restarts; corrected captures assign ownership only within the disposable instance. The native GPU/rate windows precede those failed captures. All failures and recovery are documented in METHOD.md.

## Recommended order

1. Finish broad pixel/alpha/lifecycle qualification of clear-before-overwrite, then enable it deliberately; the factory default remains unchanged now.
2. Remove the left TextureView composition pass; preserve Freeze and mode-switch safety.
3. Implement genuine render-once sharing and a GPU-native clock/glyph atlas; test each independently.
4. Replace source polling with cancellation-safe event-driven readiness; evaluate freshness and latency, not only result count.
5. Consider lifetime-safe EGLImage/renderbuffer caching. Current source+destination import medians total only about 0.15 ms in 841; useful, but not the primary multi-millisecond GPU cost.
6. Pursue GPU counters/layout-only experiments if needed after these changes. Do not start with speculative kernel/HWC/allocator rewrites or forced modifiers.

## Fresh-pixel follow-up and unresolved test-harness limit

Two additional 30-second decoded windows requested 1080p120 with clear enabled:

- 891, ordinary per-app pacing: **59.03 distinct frames/s**.
- 892, temporary producer-UID 120-Hz override: **59.90 distinct frames/s**.

Both have zero decoded duplicates/regressions and no pixel-validation failure. Producer logs confirm stream configuration and pacing target120; SurfaceFlinger confirms the temporary override. It did not remove the limit. This does not establish whether the remaining constraint is producer clock delivery, EGL pacing, marker workload or the non-displaying consumer. It is **not** a successful120-pixel qualification, and is not pooled with the normal demo's120-result/s cases. The override was removed and absence of the backdoor mapping verified afterward.

The initial six cases decoded13,140 images in total. They validate sampled monochrome identity/contrast/alpha, not arbitrary full-color/transparent images or every pixel. The next qualification improvement should instrument actual vsync callback intervals and swap/marker timing in this separate workload, rather than assume the requested rate equals production rate.

## Restoration and retained visual issue

- Factory HAL, factory APEX, services.jar, relay JNI and delivered producer APK hashes match the baseline.
- Profile0, clear=false, deadline1 restored; printk`8 4 1 7` and ignore_loglevel`Y` restored.
- Only original bootreceiver/drm/wifi trace instances remain; the temporary per-app frame-rate override is absent; no screenrecord process remains.
- Temporary E2E consumer uninstalled after archiving its results; normal full SurfaceView1080p60 demo restored.
- Two screenshots confirm advancing scene/frame counters. **The second screenshot also shows partially missing static clock glyphs on both panes.** This is retained as a baseline demo-clock artifact, not declared fixed or attributed to the camera transport. The unchanged factory HAL/producer APK were active; the clear candidate was off. The clock overlay needs its own correctness investigation and must not be treated as a precision latency instrument.

The recording artifact was independently probed: H.2641280×720,756 decoded video frames over73.533seconds (~10.28encoded fps). Thus the recording load itself was slow; it is not a60fps recording benchmark.

## Evidence

Adjacent JSON files contain all rates, native phase distributions, failures, GPU attribution, scheduler statistics, composition snapshots, pixel/lifecycle results and presentation summaries. Python analyzers are included. Raw device logs/EDID identifiers remain local.

Raw main archive:42,286,089 bytes; SHA256`53e93f619240c08a4e96d1fd32306715bc3df3452ed8ece9028aa1f4a60373ff`.
Qualification archive: 381,799 bytes; SHA256`07050be9c901bca87c035361d988f1f8f49b38643f674db60b5c490901d93904`.

Diagnostic HAL SHA256`db2f71eaaa779f7db9c092bffda0da02c0c8e6bfd159791c705e070fbf435724`; normal producer APK SHA256`0e1dfe3189416e11296c696e118ba193e2febe146a3af38fdb3b6f5364c6b27b`.

Host FrameIdentity positive/negative tests, FrameProfile serialization tests, fence-safety tests and 48 Python tests passed; native/APEX artifact and consumer APK builds passed. The APEX artifact was not staged. Full YUV/JPEG, dual-camera, protected-content and production-enforcing qualification are outside this experiment.

Follow-up archive: 2,450,967 bytes; SHA256 `592832fbd529c3f67d1ac6be96ebb5b3b033f42174cec31d0aa231ecd66009a9`.
