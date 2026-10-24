# Pi 5: clear-before-overwrite experiment

## Result

With the same linear SurfaceView destination buffers, clearing the color attachment immediately before the existing full-coverage draw reduced median HAL GPU service from **~5.0 ms to3.0–3.1 ms**. Reverse-order baseline repeated~5.0ms. Both clear-enabled short camera-only windows produced120 successful Camera2 results/s; render-engine occupancy fell from~98–99% to~79%.

This is an experimental performance result, **not sustained pixel-verified1080p120 qualification**, and not a full split-demo result. The producer mirror and clock were disabled during these measurements. The normal device HAL was restored; the new option remains off by default.

## Controlled change

`vendor.vcam.profile.clear=true` is honored only with active HAL profiling and `ro.debuggable=true`, sampled at session creation. The RGBA/RGBX output pass then executes `glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT)` immediately before its existing full-coverage textured quad. No buffer layout, resolution, shader, pacing budget or fence behavior was changed. This experiment tests clear, not the separate discard-extension alternative.

Mesa's tile-load logic uses clear/invalidate state to avoid preserving previous destination pixels. The result supports avoiding that unnecessary memory work. Dedicated tile-load/store performance counters were not captured, so it does not directly count saved memory transactions.

## Measurements

| Case | Preview/layout | Clear | HAL GPU median / p95 ms | RCL busy | Successful Camera2 results/s |
|---|---|---|---:|---:|---:|
| 701 | Surface / linear | off | 4.999 / 5.421 | 99.0% | 107.7 |
| 702 | Surface / linear | on | 3.018 / 3.219 | 79.0% | 120.0 |
| 703 | Surface / linear | on | 3.093 / 3.222 | 78.6% | 120.0 |
| 704 | Surface / linear | off | 4.991 / 5.467 | 98.4% | 94.6 |
| 705 | Texture / UIF | off | 2.481 / 2.792 | 98.4% | 88.2 |
| 706 | Texture / UIF | on | 1.745 / 2.170 | 97.9% | 96.5 |

All cases:1920×1080 RGBA,120fps target, same app/diagnostic HAL, one camera, producer mirror/clock disabled. Surface baseline/clear/clear/baseline order, then Texture baseline/clear.5s warm-up followed by10s exploratory rate window. A separate5s GPU trace follows18s of running. GPU submit→completion-IRQ is the service proxy (includes IRQ-observation timing), not CPU draw-call time. Do not sum overlapping stage medians or equate callbacks with displayed fresh pixels.

Actual metadata confirms Surface output stayed linear0x0/usage0xb00 and Texture stayed BroadcomUIF0x0700000000000006/usage0x300. All metadata getter status values were0. Logs verify the intended clear flag in each session. Clock samples before/after traces were similar (~962–994MHz). No kernel trace overruns/drops; independent scheduler-fence attribution checks had zero mismatches. Raw distributions and failed callback rates remain in the JSON files.

## Correctness and release boundary

- Native matched APEX build passed;48 Python tests passed; diff-check passed. No APEX install or platform rebuild was deployed.
- Clear-enabled1080p60 full-demo visual smoke showed the expected gradient/clock, matching displayed frame40/60 in both panes and continuing successful camera results. This is a smoke check, not a sequence-marked pixel oracle.
- Before enabling by default: exercise RGBA/RGBX pixel content and frame identity, repeated pool-buffer reuse, rapid reconfiguration, capture restart and source stall/resume, plus longer120fps runs. Include a deliberate wrong-marker negative control. Existing YUV E2E tests do not validate this RGBA optimization.
- Normal SurfaceView1080p60 full demo restored. No kernel, KMS, HWC, Mesa, allocator or AOSP framework changes were installed.

## Broader investigation map

[Ranked subsystem areas, evidence, proposed measurements and priorities](subsystem-map.md). This distinguishes GPU service, queue waits, CPU scheduling and physical display latency. The next useful work is pixel qualification of the clear candidate and removing duplicated producer/left-preview work, not blind kernel/governor tuning.

## Provenance/restoration

- Diagnostic HAL SHA:`a5f04d2e180f17f115d0872d03656952ca23a863d0b8e46b5a4f8bc4cabe2fa4`.
- Exact-hash-guarded temporary bind overlay; EXIT trap removed it and restored factory HAL SHA:`c344bea5b6c76f5d4ecceb91b9bc7065e393388b35483de6270ad86cb02ce1b8`.
- Profile0, clearfalse, deadline1; existing platform/JNI/app identities retained. Development userdebug/permissive status unchanged. The built APEX was not staged.
- APK SHA:`0e1dfe3189416e11296c696e118ba193e2febe146a3af38fdb3b6f5364c6b27b`.
- Local raw archive13,841,572bytes; SHA:`5fc79dade3f6f8494fdd1f8bfbbd5af3692e77ad38ad133b2f507a1f5cb3bcde`.
- Reproduce analysis by safely extracting the archive into `extracted/` beside the supplied analyzers. Raw device logs remain local; this directory publishes aggregates only.
