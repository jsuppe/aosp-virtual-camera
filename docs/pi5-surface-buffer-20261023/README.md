# Pi 5: why the SurfaceView HAL output pass doubled

## Finding

**The actual Camera2 output allocation changes from Broadcom UIF-tiled to linear when switching from TextureView to SurfaceView.** The HAL render-code path is unchanged. The roughly twofold GPU output service difference repeats in Texture/Surface/Surface/Texture order at120fps, and remains at60fps where the GPU is not saturated. GPU clock samples are similar, not halved.

This is strong evidence for the changed destination/scanout path as the primary explanation, not proof that every nanosecond is attributable solely to memory layout: this experiment changes consumer/composition along with layout. A layout-only microbenchmark has not been run.

SurfaceView still saves the separate Android UI texture-composition pass. Removing one pass while making another more expensive is consistent with an overall improvement. At60fps, total render-engine occupancy fell from61.9% to49.4%, despite HAL job service increasing from2.302 to4.607ms.

## Actual allocation metadata

A bounded debug/profile-only log queries GraphicBufferMapper on imported HAL destination handles. All getters returned status0; unsupported metadata is not mistaken for modifier0.

- TextureView: usage `0x300`, modifier `0x0700000000000006` (**Broadcom UIF**).
- SurfaceView: usage `0xb00`, modifier `0x0` (**linear**).
- Both:1920×1080, fourcc `0x34324241` (AB24), reported allocation size8,305,772bytes.
- The extra usage bit `0x800` is HW_COMPOSER. In this matched minigbm/Mesa stack, it becomes BO_USE_SCANOUT → GBM_BO_USE_SCANOUT → PIPE_BIND_SCANOUT. Without explicit modifier negotiation, V3D forces linear allocation.

[Driver source path and locations](driver-path.md); [metadata samples](metadata.json).

## Matched observations

| Run | Preview | Target fps | Output layout | HAL GPU median / p95 ms | RCL busy | Successful results/s |
|---|---|---:|---|---:|---:|---:|
| 601 | TextureView | 120 | UIF tiled | 2.484 / 2.818 | 98.3% | 84.1 |
| 602 | SurfaceView | 120 | linear | 5.003 / 5.471 | 98.5% | 107.8 |
| 603 | SurfaceView | 120 | linear | 4.943 / 5.375 | 97.2% | 108.0 |
| 604 | TextureView | 120 | UIF tiled | 2.412 / 2.790 | 98.2% | 87.0 |
| 605 | TextureView | 60 | UIF tiled | 2.302 / 2.325 | 61.9% | 60.0 |
| 606 | SurfaceView | 60 | linear | 4.607 / 4.861 | 49.4% | 60.0 |

The same SurfaceView-capable APK was used for all cases. Producer mirror and clock were disabled, while the animated producer scene remained enabled. One camera, RGBA,1080p. First5s warm-up, next10s used for exploratory callback/submission rates. Separate5s kernel GPU trace after18s; GPU clock sampled before and after that trace, not continuously during it. Native profiling and metadata logging were enabled in all six cases; only consumer and selected fps changed. Metadata logging occurred on initial buffer import, outside warmed measurement windows.

These short windows investigate a specific GPU-pass regression; they are not sustained fresh-pixel120fps qualification. Successful callbacks are not independent pixel delivery or photon latency. Previous longer factory-HAL results remain in [the SurfaceView report](../pi5-surfaceview-20261023/README.md); do not pool the two experiments.

GPU service is V3D hardware-submit→render-completion-IRQ time, not Java draw-call time or GPU queue waiting. Job attribution independently cross-checks scheduler fences: zero mismatches across all six cases. Trace overrun/drop counters are zero. Clock samples ranged960.2–989.5MHz; temperatures47.95–50.7°C. These snapshots do not exclude every transient frequency change.

## Serial-console question

The board uses115200baud with verbose kernel-console configuration (`ignore_loglevel`, `loglevel=8`, `keep_bootcon`). Heavy printing can perturb scheduling; enabling an idle UART is not the same as saturating it. At8N1, nominal link capacity is11,520bytes/s.

Within the exact six5s GPU trace intervals, timestamp-matched kernel-console capture contained224bytes in601 and603, and zero in602,604,605,606. The two224-byte cases each contain a duplicated112-byte logd diagnostic. Thus the ~2× GPU service difference also occurs in completely quiet recorded kernel-console windows; console traffic is not a credible primary explanation for this specific difference. This is volume evidence, not a serial-enabled/disabled A/B or a universal claim of zero UART overhead. Kernel timestamps need not equal physical UART arrival times. Startup/error bursts can still cause hiccups. Console settings were not changed.

## Next optimization to test

The HAL currently imports the destination as an EGL renderbuffer, draws a full-coverage texture-sampled quad, and returns a fence. That is real pixel rendering, not just queueing an existing framebuffer.

Mesa's render path may load old destination tiles unless a clear/invalidate marks their contents disposable. The HAL currently does neither before overwriting the output. A bounded **clear/discard-before-full-overwrite** trial is the next low-scope optimization, with fresh-pixel correctness checks and identical output-buffer layout. It was not applied in this diagnostic change. Do not force a UIF modifier into a display buffer without verifying compositor/display support.

## Validation and restoration

- Native module and matched APEX artifact built;48 Python tests passed; diff-check passed. The APEX artifact was not staged/installed.
- A temporary, exact-hash-guarded library bind overlay supplied the diagnostic HAL. Only allocation metadata logging was added; rendering code was not changed.
- Diagnostic library SHA:`e481110153985f872b3fd583c907a7e2ae63383bcbbef893ed7b2e5f210adbfe`.
- EXIT restoration removed the overlay and verified factory HAL SHA:`c344bea5b6c76f5d4ecceb91b9bc7065e393388b35483de6270ad86cb02ce1b8`.
- Profiling reset to0, deadline setting1. New SurfaceView app retained, normal mirror+clock1080p60 restored. Development userdebug/permissive SELinux unchanged.
- APK SHA:`0e1dfe3189416e11296c696e118ba193e2febe146a3af38fdb3b6f5364c6b27b`.
- Raw archive retained locally:11,914,871bytes, SHA:`aeb5b63565a33daf206847aa4f4a0d871ce4e11a604fb0a4c421db795d2b9a88`.
- To reproduce analysis, extract that archive safely under `extracted/` next to these Python analyzers. Raw device logs are not published; this directory holds aggregates.
