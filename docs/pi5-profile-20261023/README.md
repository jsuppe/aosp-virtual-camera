# Pi5 1080p120 profiling — 2026-10-23

## Outcome

1080p60 remains the working default. This run identifies graphics pressure and
source-completion deadlines as important constraints; it does **not** establish
a fundamental Pi5 hardware limit or qualify sustained 1080p120.

One physical Pi5, Android17 aosp_rpi5-cp2a-userdebug, ARM64, 1920x1080 RGBA,
120Hz-capable display. Each diagnostic case lasted70s; analysis uses a fixed60s
window after5s warmup. No concurrent screen recording/builds during these windows.

| Run | Target | Direct mirror | Clock | Producer submissions/s | Successful Camera2 results/s |
| --- | --- | --- | --- | --- | --- |
|201|60|on|on|60.00|59.98|
|202|120|on|on|74.35|8.47|
|203|120|off|on|119.98|76.80|
|204|120|off|off|120.00|89.32|

These are **not independently decoded distinct-pixel FPS or physical display
latency**. Native source identities supplement callback counts. No trace overflow,
missing producer timestamps, negative callback latencies or successful native
source-sequence regressions occurred in these four windows.

## Where requests fail

Run202:6,540 native attempts,508 successes,3,954 source-fence timeouts and2,078
no-fresh-source outcomes. The one-frame budget at120fps is about8.33ms and is
shared across source acquisition and producer-fence waiting. A late source can
leave very little budget for its completion fence. This amplifies the throughput
loss. The experiment did not relax freshness, fence safety or the deadline.

Removing the extra direct mirror substantially improved producer cadence and
successful results. Removing the dynamic clock improved results further. The
clock-update CPU-elapsed interval (Canvas plus texture update, not just upload)
had median11.648ms in run202,1.775ms in203,0.022ms in204. Asynchronous graphics
pressure can surface inside API calls; these are not GPU execution timers.
Run204 still had1,841 source-fence timeouts among7,199 attempts.

Temperatures at case boundaries were47.4–49.6°C. This is not evidence of thermal
throttling as the primary cause; clocks/throttle state and GPU execution were not
fully profiled. Successful output preparation CPU elapsed is also not proof that
the GPU blit costs nothing.

At60fps, producer timestamp to successful Java Camera2 result callback was
median16.229ms,p9516.464ms. This ends at metadata callback receipt, not image
availability or photons. The120fps latency samples include only successes and
have strong survivorship bias. Equal photographed clocks do not prove zero latency.

## Rejected offscreen-cache trial

Same candidate APK, same scene, clock+mirror enabled,120fps target:

| Run | Candidate | Producer submissions/s | Successful Camera2 results/s |
| --- | --- | --- | --- |
|301|original render path|75.73|7.72|
|302|offscreen source-cache path|72.98|0.00|

The candidate added an offscreen scene texture/copy on the source path; the direct
mirror still performed its original draw. It did not achieve a genuine shared
single-render output for both windows. At60fps it passed a visual scene/color
smoke check, but at120fps all6,592 native attempts failed (4,379 source-fence
timeouts,2,213 no-source). It is rejected and removed from the delivered source.
No claim that a correctly engineered render-once architecture cannot help follows
from this failed prototype. An earlier black-screen candidate and its correction
are retained in local evidence.

## Scrcpy observation

The earlier user-triggered scrcpy session selected c2.android.avc.encoder (software)
and audio encoding. It adds work and can perturb this graphics-heavy test.
Producer progress subsequently resumed. There was no controlled scrcpy on/off
comparison, so overload is plausible, not a proven cause of the reported freeze.

## Deployment and recovery

The signed diagnostic APEX8 built with matching factory keys. First direct pm
attempt omitted --apex and failed as an APK session; corrected --apex --staged
attempt failed in apexd/dm-linear device lookup with EBUSY. APEX8 never activated.
This is an unresolved update-path blocker, not a successful staged update.

Profiling used a temporary hash-guarded bind overlay of the implementation library,
extracted from the signed package. Existing userdebug/permissive policy was not
changed. The overlay was removed and factory implementation SHA256
c344bea5b6c76f5d4ecceb91b9bc7065e393388b35483de6270ad86cb02ce1b8 restored.
Factory APEX7 remained SHA256
d655cf4080f7a2a8b64d4af65ba4bea460a210a2f9d1d8daf15e67b89209622c.
Profiling property was reset to0. The restored app APK SHA256 is
75fdea8f223bdd0a554050a61c32f11e6ddc822cea4e420cc3bb12219e219a9d.
The first post-install launch restored VGA saved state; force-stopping just the
comparison app and relaunching explicitly selected1080p60. Native CSV dimensions
independently confirm1920x1080 in all six measured cases. This is not an
Enforcing/security qualification. Services/JNI hashes remained unchanged.

## Evidence and next bounded step

Machine-readable summaries and analyzer are adjacent. Local raw archive
raw-final.tgz:3,488,254bytes, SHA256
cdb4424e7c908180c4d0df89f0098a17efa380490b29ddd60e063b7c3fceb678.
It contains the app/native CSVs; it is not embedded in this source repository.
The analyzer expects extracted/ relative to itself; extract only trusted regular
CSV files/directories with safe path handling. Earlier empty task-reuse attempts
were excluded, not treated as measurements.

Diagnostics default OFF, bounded16,384 rows, export after stream close. Native
recorder and Java recorder host tests passed;48 Python checks and five existing
host suites passed. Both target apps build. No new end-to-end pixel oracle or
all-mode lifecycle qualification is claimed by this profiling run.

Next: GPU fence/presentation tracing and an actually shared render output or
low-cost procedural/static clock, tested independently. Then evaluate whether
source readiness scheduling can improve success while preserving fence safety,
freshness and bounded latency. Do not just extend waits and call the result
low-latency120fps. Keep1080p60 as the comparison baseline.
