# Pi5 1080p120: fence deadlines and GPU-job attribution

## Main finding

The current displayed camera pipeline is **render-engine limited**, with additional
polling delay. This is a workload/driver/display-path finding, not proof that the
Pi5 silicon cannot handle1080p120 with a more efficient pipeline.

With mirror and dynamic clock disabled, five independent5-second trace windows
showed **97.2–98.8% V3D RCL submit-to-completion-IRQ coverage**. This measures
render-job elapsed occupancy, not a hardware shader-active-cycle counter or the
utilization of every GPU engine. Binning coverage was only6.0–6.9%.
There were no trace overruns/dropped events.

Matched GPU jobs separate the costs that CPU API timing previously hid:

| Origin | Median render-job submit-to-IRQ service |
| --- | --- |
| Producer, normal ball scene |3.25–3.27ms|
| Producer, minimal solid/ID-stripe scene |2.26–2.29ms|
| Camera HAL output pass |2.37–2.59ms|
| Android RenderThread |4.83–4.97ms|

These are actual hardware-submission-to-IRQ intervals, excluding prior GPU queue
waiting. RenderThread is the observed submitting thread; the label is not a
claim that all compositor/display work was traced or attributed. In these runs
it is the largest individual pass. Its roughly5ms pass competes with producer
and HAL passes on the same render engine. Do not sum medians as an exact measured
per-frame critical path: the groups have different job rates and buffering.

This shifts the next optimization priority toward **the consumer display path**:
compare the existing TextureView against a separate SurfaceView camera surface,
then verify actual HWC composition/scanout rather than assuming an overlay is used.
A non-displaying consumer is also needed to separate camera transport capability
from showing its output. Preserve the real Camera2 route in both comparisons.

## Controlled throughput/deadline experiment

Same diagnostic APK/library,1920x1080RGBA,120fps requested,mirrorOFF,clockOFF.
Each case:70s collection, analyze60s after5s warmup; kernel tracing occurs ONLY
after that interval. No recording or concurrent builds during measured windows.
Two small shell status reads occurred during the sequence and are not hidden.
Source dimensions are asserted from native samples.

| Run | Scene | Availability+readiness budget | Producer submissions/s | Successful Camera2 results/s | Callback latency median / p95 |
| --- | --- | --- | --- | --- | --- |
|401|normal|1 frame|120.00|101.10|8.91 /16.74ms|
|402|minimal|1 frame|120.00|99.65|16.52 /16.98ms|
|403|minimal|2 frames|116.80|105.40|16.91 /25.12ms|
|404|normal|2 frames|109.30|92.47|18.88 /26.88ms|
|405|minimal repeat|1 frame|120.00|107.62|10.71 /16.94ms|

The repeated minimal baseline varies enough that105.4/s with a longer budget is
not a convincing gain over99.65–107.62/s with the original budget. Longer waits
also lowered producer cadence and worsened upper latency. **No deadline change
is being promoted as an optimization.** Compared with prior experiments, absolute
rates vary; report this series as a matched diagnostic cohort, not an improvement
delivered to the normal app.

Callback latency means vendor producer/BufferQueue timestamp to Java successful
Camera2 result callback. It is NOT image availability, end-to-end display latency,
or a distribution of failed frames. Successful-result FPS is NOT independently
decoded fresh-pixel FPS.

## CPU scheduling versus fence polling

Across these trace windows, runnable-to-CPU p99 was approximately:
- producer render thread:0.14–0.18ms;
- HAL Binder threads:0.18–0.27ms.

Rare maxima reached about1.66ms. This does not establish CPU preemption as the
dominant bottleneck; GPU queue waits were several milliseconds and sometimes
well above a frame interval.

For fences that signaled after source acquisition, kernel signal-to-observation
median was1.04–1.28ms,p95 roughly1.92–2.03ms. This matches the existing up-to2ms
CV polling granularity. It is measurable secondary overhead. A cancellable
fence-event wait is a future candidate, but does not remove a saturated render
pass and must preserve cancellation, freshness, buffer ownership and fence safety.

Most deadline failures were genuinely unsignaled when queried:
401:1133/1134;402:1221/1223;403:21/22;404:286/287;405:734/737.
The occasional already-signaled snapshot is a decision/query race, retained in
the data. Successful signal times later than the nominal deadline also exist:
the original loop checks readiness before checking timeout, so this policy is
not an exact hard cutoff. Diagnostics preserve that behavior.

Native schema2 appends source acquisition,deadline,fence signal,fence observation
and fence status. A sync_file_info query is made AFTER the readiness decision and
never substitutes for waiting. Its ioctl/allocation overhead is included in every
case. No signal timestamp is inferred for an unsignaled fence. A signal before
the BufferQueue producer timestamp can be legitimate; do not confuse it with a
negative producer-to-consumer latency or discard it silently.

## Attribution validation and limits

RCL command addresses join originating ioctls to hardware submissions and IRQs.
An independent DRM scheduler fence-context/sequence join cross-checks origin:
run4011443,4021561,4031572,4041410,4051560 checks; **zero disagreements**.
Initial boundary jobs without an observed origin are counted separately (1–4).
GPU summaries cover later5-second trace windows, not exactly the throughput window.

Temperature boundaries remained47.4–48.5°C. GPU clocks and detailed bandwidth/
shader counters were not sampled, so compute versus memory/resolve cost and clock
policy remain unresolved. Do not claim a thermal or immutable hardware ceiling.

The minimal shader is a solid background with a16-bit changing stripe. Screenshots
confirmed real Camera2 pixels, but this is not yet a corruption-checked per-image
oracle with a wrong-source negative control. No fresh-pixel120fps qualification
or long-duration soak is claimed.

## Artifacts, tests and restoration

Source baseline:f136dd8. Diagnostic implementation:
693a8a2eecd9338be0d679d35f7ae5268640aca369637fa476649634eb535d94.
Diagnostic APK:
3d592f343dd3c5a3da7f1858ac6aee37a2b031e5936597c240349b72671bd9e6.
Matched A17 module/APEX+both-app build passed28seconds;48 Python checks,native
schema/bounds/timing test and five host regression suites passed.

Direct debug library bind overlay used on the existing permissive userdebug base;
not an APEX activation/security qualification. The runner's EXIT handler restored:
- factory HAL implementation c344bea5b6c76f5d4ecceb91b9bc7065e393388b35483de6270ad86cb02ce1b8;
- normal APK75fdea8f223bdd0a554050a61c32f11e6ddc822cea4e420cc3bb12219e219a9d;
- profile0,deadline override1 and1080p60.
The private tracefs instance was removed; preexisting bootreceiver/drm/wifi
instances were preserved. Normal live output is checked separately.

Raw local archive raw-results.tgz:21,118,815bytes, SHA256
7e0c3876a5a7fec3cb4673e7bd8ca5aa32278060435734144f337ae719dea0ba.
Contains CSVs,raw kernel traces,trace buffer stats,runner log and diagnostic
payloads. Raw system traces are not committed here. Adjacent aggregate JSON and
analyzers expect safely extracted files under extracted/. Run400 is a smoke run,
not part of the fixed-window analysis. Earlier truncated UART query is retained.

Diagnostic controls remain opt-in. The2-frame override requires both active
profiling and ro.debuggable and is clamped to1..2; profile-off behavior remains
one frame. The running normal APK/HAL do not contain this experiment.

## Next bounded experiment

1. Replace only the Camera2 consumer's TextureView with a SurfaceView test path.
2. Compare matched1080p120 runs and confirm the RenderThread pass is reduced,
   not just hidden or replaced by another GPU composition pass.
3. Add per-image identity/latency verification with its overhead measured.
4. Only then test cancellable fence-event waiting versus2ms polling.
