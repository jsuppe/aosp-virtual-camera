# Method and measurement boundaries

One Raspberry Pi5, Android17/API37 ARM64, same 1920×1080@120Hz display mode throughout. This is an engineering experiment, not a hardware maximum or production qualification.

## Workloads

- **Pixel qualification:** public Camera2 → RGBA ImageReader, camera100 only, four reader buffers promptly drained. Producer renders a changing128-bit producer/run/generation/sequence/CRC marker. Every RGB channel is independently decoded; sampled alpha must be255. Each decoded identity must be strictly increasing within its generation and match the expected producer/run. This checks sampled cells, not every pixel against a golden image. Marker drawing and CPU readback differ from the normal demo; its FPS must not be substituted for SurfaceView FPS.
- **Normal demo matrix:** same delivered producer APK, same scene; isolate clear, direct mirror, clock, resolution, target rate, preview surface and minimal shader. Fixed60s analysis window beginning5s after first producer sample. No concurrent screenshot/readback/build during baseline windows.
- **Native GPU attribution:** a separate5s kernel trace follows each throughput window. Join submission command-list addresses to hardware starts and render IRQs; independently cross-check scheduler-fence origins. Hardware-start→IRQ includes interrupt-observation delay; submission→start is queue/dependency delay. Neither is Java API execution time.
- **Presentation:** separate10s Perfetto captures for selected workloads, with SurfaceFlinger FrameTimeline and a private ftrace instance. Presentation is not photon detection.
- **Load controls:** bounded1280×720 screen recording at4Mbps is an additional load, not an identical reproduction of scrcpy's video+audio/network path. Quiet-console trial preserves/restores both printk and ignore_loglevel; it does not remove the UART driver or reader.

## Diagnostic changes

The temporary HAL library adds default-off CPU substage timestamps. The clear candidate is still enabled only by a debug/profile property. No permanent kernel, Mesa, allocator, HWC, KMS or framework changes are part of this experiment. The matching APEX was built, but only a hash-guarded temporary library bind overlay is used; that does not qualify staged APEX updates.

Schema3 HAL timestamps separate lock/context setup, source import, destination import, GL draw submission, fence/flush submission and cleanup. All are **CPU elapsed** intervals and may include blocking in the driver. Only native job tracing estimates GPU execution. The profile buffer is bounded to16,384 samples, with explicit dropped counts.

## Latency endpoints

- Producer submission cadence is not fresh camera-output cadence.
- Successful Camera2 metadata callbacks are not independently decoded unique pixels or physical presentations.
- Producer timestamp→callback includes upstream timing semantics and only successful requests (survivorship bias).
- RGBA `receipt_ns` is entry into the image-available callback, before acquisition/decoding; it is not the completion time of CPU pixel validation.
- FrameTimeline/present fences describe software/display pipeline events, not monitor processing or emitted photons.
- Different stages overlap; summing their medians produces a misleading end-to-end number.
-120Hz gives an8.333ms refresh interval, not a universal minimum end-to-end latency.

## Scope and remaining validation

Tested single-camera RGBA path and mode/lifecycle transitions. This does not qualify dual-camera load, YUV/JPEG, protected content, all alpha/blend combinations, arbitrary resolutions, suspend/resume, day-long thermal behavior or staged updates. Optical latency requires a high-speed camera/photodiode with identifiable stimulus, appropriate exposure and scanout-position accounting.

Raw logs, trace files and EDID/device identifiers remain local. Only sanitized summaries, scripts and source are published.

## Why a full-frame pass has a cost

1920×1080 RGBA8 is8,294,400 bytes of visible pixels. At120fps, one complete read or write is about0.995GB/s of logical pixel traffic. A source read plus destination write is about1.99GB/s before extra destination loads, cache effects, tiling, scanout or other passes. These are arithmetic workload estimates, **not measured DRAM bandwidth**. A queued framebuffer is cheap only when an already-complete, compatible buffer can be passed directly through; the current HAL actually renders a new output buffer and must honor ownership/fences.

A TextureView can use a GPU-friendly tiled camera output but then needs app composition. A SurfaceView can eliminate that app composition for the right pane while selecting a scanout-compatible linear destination, making the remaining HAL pass slower. Both effects can occur simultaneously. The relevant objective is total critical-path work and fresh presentations, not the duration of one isolated pass.

## Preserved diagnostic failures

1. `modetest` initially failed dynamic linking because `libdrm.so` is packaged in the Mesa APEX, not a normal library search path. A diagnostic-only `LD_LIBRARY_PATH` fixed the read-only query.
2. The wrong-run negative test correctly rejected producer819 when99999 was expected. The shell checker incorrectly matched unescaped `/` against Android's JSON `\/`. Host JSON parsing confirmed the rejection and all positive/lifecycle checks before performance testing proceeded. The original failure remains in the local log.
3. Initial private-instance Perfetto captures encountered root-owned tracefs files. `traced_probes` UID9999 hit EACCES and a fatal `ClearTrace` check. FrameTimeline-only data from those captures is not used as a clean scheduling/performance comparison. The native GPU and60s throughput windows precede these captures. Follow-up captures grant ownership only within the disposable private instance and verify the probe PID remains stable; no parent/global tracefs permissions or other instances are changed.

## Reproducing the analysis

Extract the verified local archives into `extracted/` beside the analyzers, preserving relative paths. Run `analyze-performance.py`, `analyze-trace.py`, `attribute-gpu.py`, `analyze-system.py`, `analyze-composition.py` and `analyze-qualification.py` with Python3. For presentation analysis, install the official Perfetto trace processor separately and set `TRACE_PROCESSOR` to its executable. To reproduce console byte counts, provide the matching `console.raw` beside `analyze-system.py`; missing console input is reported unavailable, not zero.

Device scripts are an exact, hash-guarded experiment record, not a portable installer. They expect the matching diagnostic library, APK and board paths described here. The initial832/835 captures used presentation-original.cfg;838 used the FrameTimeline-only presentation.cfg after the permission failure. The later presentation-tests.sh uses the original combined config with corrected private-instance ownership. The UID guard in fresh120.sh is deliberately specific to this baseline.

The last two sampled-pixel cases are in fresh120.json and the separate follow-up archive. The per-UID120Hz override did not raise decoded throughput above60fps; it was removed. Guard checks initially stopped this follow-up before mutation because package-name filtering also matched producer2 and because an unanchored Backdoor search matched debugDisplayModeSetByBackdoor=false. Both guard matches were narrowed to the intended identity/field before execution; no existing override was replaced.
