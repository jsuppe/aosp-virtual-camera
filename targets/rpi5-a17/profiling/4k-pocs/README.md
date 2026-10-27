# Opt-in UHD performance experiments

Only the measured app-only pair (left SurfaceView + preview-sized reference) is
now the normal UHD demo default; other experiments remain off. Use the matched Pi A17 base
and archive APK/HAL identities. See [plan](../../../../docs/pi5-4k-pocs-20261027/PLAN.md).

Cached-clock/render-once and higher-rate experiments require a positive
`profile_run` extra. Diagnostic runs default every optimization off unless set
explicitly, preserving the baseline; normal UHD defaults the two app winners on. Start a fresh
producer process when changing profiles: mode descriptors are registered once.

- `left_surface=true`: left SurfaceView instead of TextureView.
- `preview_sized=true`: reference buffer uses visible pane dimensions instead
  of3840x2160. Camera buffer remains UHD. Works with either left output type.
- `cached_clock=true`: pre-rasterize one cycle of clock textures, then bind the
  appropriate texture each frame. At30/60/120fps this retains about15/30/60MiB
  of RGBA texels; temporary Canvas bitmaps are recycled after each upload.
- `render_once=true`: render scene+clock into one generation-local UHD FBO;
  sample into the camera window, then sample the exact same texture into the
  reference window. It is not zero-copy: camera and reference blits still cost
  GPU bandwidth. Adds about31.6MiB of texture storage.
- HAL `vendor.vcam.profile.clear=true`: existing debug-only full-coverage RGBA
  output clear, effective only with positive native profile run and debuggable base.

`profile_run` also selects an explicit `4k-perf` descriptor allowing higher UHD
request rates for experiments (30/40/45/50/60/120). Normal launches retain the UHD30 ceiling. This
is a diagnostic capability advertisement, not a throughput guarantee. Producer
pacing still follows the physical display's Choreographer clock.

`run-case.sh` runs one bounded window on-device and archives traces. It assumes
the known matched diagnostic HAL is already temporarily mounted using the prior
investigation's hash-guarded overlay script; it does not install that overlay.
Restore it and reset vendor profile properties after testing. Never bind-mount an
unmatched native library or bypass the guard.

`matrix.sh` defines isolated cases; `analyze.py EVIDENCE_DIR` reads
`EVIDENCE_DIR/extracted/`, requires untruncated CSVs/window coverage and computes
60-second metrics after15seconds warmup. Keep captures, recording and builds out
of the rate window. The subsequent5-second GPU trace is a separate sample.

Single EGL thread owns the FBO, textures and windows. Queue ordering in that
context protects reuse across draws; no external acquire/release fence is
removed or weakened. Texture/FBO resources are retired on stream replacement.

The bundled `overlay.sh` and `trace-window.sh` preserve the prior investigation's
hash-guarded base/payload contract and `/data/local/tmp/vcam-investigation` paths.
Upload those helpers and the matching diagnostic library there; upload the case
scripts to `/data/local/tmp/vcam-4k-pocs`. The native module is built from the
matched integrated A17 tree (`m android.hardware.camera.provider-V1-virtual-impl`).
A newly built binary with a different hash requires a new matching-base audit,
not deleting the guard. The exact tested payload is retained with local evidence.

Measured candidates and failed interactions are in the
[full result report](../../../../docs/pi5-4k-pocs-20261027/README.md). Cached clock
was also tested with the cheaper preview; it did not resolve the clock-glyph
artifact and remains default-off. No strategy is justified by a callback count
alone as a claim of correct scanout or optical latency.
