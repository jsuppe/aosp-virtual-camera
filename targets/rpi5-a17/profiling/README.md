# Opt-in Pi5 frame-stage profiling

Diagnostic support is disabled by default. Normal frame/fence/readiness policy is
unchanged. A separate [debug-only deadline experiment](deadline-experiment) permits
an explicit bounded two-frame wait budget while profiling on a debuggable build. The HAL and app keep bounded in-memory samples (16,384 rows per trace)
and write CSV only after closing the measured stream. A nonzero dropped count
invalidates a full-window claim; retain it rather than silently truncating data.

## Components

- `hal/core/FrameProfile.h`: native capture stages and explicit outcomes.
- `FrameTrace.java` in the target app: producer stages and Camera2 callback samples.
- `run-cases.sh`: matched-Pi benchmark runner. Requires expected service-binary
  **and implementation-library** hashes; the binary alone can remain unchanged
  when the implementation changes.

Select the matched Pi17 ARM64 base and verified diagnostic packages first. Keep
normal signing, kernel and SELinux settings intact. Native recording is enabled
at camera-session creation by `vendor.vcam.profile=RUN_ID`; zero disables it.
App extras `profile_run`, `mirror` and `clock` select diagnostic recording and
independent mirror/clock controls. Both mirror and clock default to enabled.
These extras are diagnostic controls, not additional normal app UI.

The runner clears only the comparison app's activity task when opening each
case to avoid reusing a different top activity. The foreground producer service
stays registered. Runs201–204 are1080p60 all-on,1080p120 all-on,120 mirror-off,
and120 mirror+clock-off. Each remains open70seconds, with analysis using60seconds
after5seconds warmup. No screen recording, manual mode changes or concurrent
builds should occur during a run. Record actual display cadence and device state.

## Native outcomes and timing meanings

CSV `failure` values:0 success;1 destination-fence duplication failure;
2 invalid buffer import;3 destination-fence timeout;4 cancellation;5 no fresh
source;6 source-fence timeout;7 stale/invalid source configuration;8 no filled
output. Invalid requests rejected before the single-request path are outside
this recorder; inspect admission errors separately.

Times are CPU elapsed intervals, **not GPU execution timings**:

- `import_ns`: fence preservation and handle import.
- `destination_wait_ns`: destination acquire-fence waits.
- `pace_ns`: request pacing.
- `source_wait_ns`: fresh source acquisition/readiness waiting.
- `source_fence_wait_ns`: producer-completion-fence waiting.
- `output_ns`: output preparation/render API calls, including any internal waits.
- `callback_ns`: notification, metadata construction and result callback work.
- `cleanup_ns`: remaining scope cleanup, including releasing the source reservation.

Source sequence identities are HAL submissions, not decoded pixel markers.
Producer `draw_end - overlay_end` measures CPU draw submission, not GPU time.
Camera callback time minus vendor producer timestamp ends at Java Camera2 result
receipt, not image availability or display presentation. Keep missing timestamps,
negative values and boundary effects visible. Do not compare sensor pacing
metadata as if it were a real physical exposure timestamp.

## Deployment finding

The signed diagnostic APEX8 builds and shares the factory payload key/container
certificate. On this Pi17 base, a correct staged install was rejected by apexd
because dm-linear device lookup returned EBUSY. This is NOT a qualified Pi17
staged-update path. For direct `pm install`, include `--apex`; unlike the adb
client's APEX handling, omitting it creates an APK install session.

The physical profiling run uses a temporary, hash-guarded bind overlay of the
implementation library extracted from the verified signed package, followed by
restarting only the virtual-camera HAL. The factory APEX file remains unchanged.
This debug deployment uses the board's existing permissive baseline; it is NOT
an Enforcing/security qualification or an APEX activation test. Remove the bind
overlay, verify the original library hash and restart the HAL after profiling.
Do not publish a successful staged-update claim based on this debug run.

## Schema2 fence snapshots

Native schema2 appends source_acquired_ns, deadline_ns, fence_signal_ns,
fence_observed_ns and fence_status. Signal timestamps come from sync_file_info;
unsignaled fences have no inferred completion time. The snapshot is taken after
the readiness decision and is diagnostic-only. Its ioctl/allocation overhead is
included in the measured path. See the deadline experiment report for timing
interpretation and snapshot races.

App extra minimal_scene=true selects a solid shader with a16-bit changing stripe
only when profile_run is positive. For a minimal workload also disable mirror
and clock. This stripe alone is not a full end-to-end correctness oracle.
