# 4K performance POCs

Baseline: d29a5c8 Pi5 A17, UHD3840x2160 RGBA full split, clock+mirror on,
right SurfaceView, left TextureView, HAL clear off. Existing permissive reference
base, no kernel/HWC/allocator/governor changes. Preserve delivered APK and HAL hashes.

Initial five independent candidates, all default-off and opt-in during comparisons
(final app-only promotion is recorded in README.md):
1. HAL clear-before-overwrite: avoid destination tile loads; existing diagnostic
   HAL switch, unchanged fences and full-coverage RGBA shader.
2. Left SurfaceView: eliminate app RenderThread TextureView composition.
3. Reference buffer sized to visible preview: avoid shading an additional UHD
   image just to downscale it. Camera input/output remains UHD. Reference is not
   a native-resolution pixel comparison in this configuration.
4. Precompute all nominal clock frames as GL textures: same clock appearance,
   no per-frame Canvas raster/upload. Trades bounded extra GPU memory and startup
   work for steady-state work. Not a change to the scene or frame pacing.
5. Render-once FBO: draw the scene once to a UHD texture, sample that same frame
   to camera and mirror. Adds a camera blit; may lose if shading savings do not
   offset bandwidth. One EGL owner/context, GPU ordering, generation-local lifetime;
   no imported external buffers, fence bypass or CPU pixel copy.

Method: baseline + each isolated candidate + baseline repeat; same APK,
requested30fps, 15s warmup followed by60s measured device-monotonic window.
Collect producer/Camera2 per-frame traces, failed results, enqueue-to-result age,
interval tails, native source identity where available, clocks/temperature.
No screen recording or builds in measurement windows. Subsequent short GPU trace
can attribute cost but is not represented as simultaneous with the rate window.
Then combine positive candidates, compare repeats, request60/120 only via an
explicit diagnostic profile, and find the best tested rate (not universal hardware maximum).

Correctness: inspect scene/clock/orientation, Freeze, size switches/reopen, decode
independent RGBA/YUV frame identities with wrong-run rejection. Test combined
best under full split load with a bounded pixel oracle if feasible; distinguish
readback's extra cost from uninstrumented performance. Preserve failed runs.
Report callback, decoded fresh-frame, presentation and optical evidence separately.
Restore temporary HAL overlay/properties after experiments. Retain POCs opt-in;
only promote measured winners with explicit correctness coverage and clear limits.

## Initial evidence and next matrix

First isolated60-second windows: baseline4.58results/s, clear8.67,
left SurfaceView10.00, preview-sized reference11.20, cached clock0,
render-once0. Both zero-result runs have continuous capture attempts and
source-fence timeouts, not absent demand. Keep them as failed POCs.

Next: clear+left, clear+size, all three positives at30, then all three at60.
Reject cached-clock/render-once from this first combination. Fixed display mode
is1080p60; source vsync prevents interpreting requests above60 as delivered rate.
An initial collection error was A17 am stopservice returning1 after success;
trace files survived and the baseline was recovered, not rerun/discarded.
