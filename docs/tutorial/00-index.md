# Building a Virtual Camera on Android — A Tutorial

This is a guided tour through the Android graphics and HAL stack, taught by
building one real thing: a **virtual camera** whose "sensor" is *another app*.
The current implementation supports four producer slots, exposed as Camera2 IDs
`100`–`103` while registered with the HAL. Consumers use normal Camera2 APIs,
subject to permissions and supported stream configurations; producers use a
custom, integrated platform API. By the end you'll understand how a frame
travels across the platform/vendor boundary into a camera output buffer.
The RGBA fast path uses a GPU blit without CPU pixel copying or RGBA→YUV
conversion; that does **not** mean zero CPU work, zero waits, or zero GPU copies.

**Reviewed 2026-10-24 against source baseline `9b3cb10188ec43e04b1451582e6e60ba20500f88`
on `pi5-a17-60fps-20261023`.** See the [review record](REVIEW-20261024.md)
for scope, corrections, and validation limits.

The [top-level README](../../README.md) contains architecture and historical
bring-up notes; older examples there are not all current deployment instructions.
Use this tutorial's source links and target-specific integration guidance. This tutorial is the slower path: **one concept per lesson**,
each building on the last, each grounded in the real code in this repo. You
read and understand rather than build from scratch — every lesson points at the
actual files and explains why they are shaped the way they are.

## Who this is for

You write Android apps, or C++, and you're comfortable reading code — but the
camera stack, gralloc, Binder/HAL plumbing, APEX, and device-tree integration
are unfamiliar. No graphics or kernel background assumed. Each term is
introduced the first time it's needed.

## The arc

Each lesson teaches a concept and shows where it lives in the system. They are
meant to be read in order; later lessons assume the vocabulary of earlier ones.

| # | Lesson | The concept you'll learn |
|---|--------|--------------------------|
| 1 | [The camera as a service](01-camera-as-a-service.md) | How Camera2 → cameraserver → HAL works; what a HAL *is* |
| 2 | [Graphics buffers & gralloc](02-gralloc-buffers.md) | Zero-copy buffers, usage flags, the mapper, why frames go black |
| 3 | [Surface & BufferQueue](03-surface-bufferqueue.md) | The producer/consumer conveyor every Android frame rides on |
| 4 | [Binder & a control plane](04-binder-control-plane.md) | AIDL, servicemanager, registering a producer, handling death |
| 5 | [The Treble wall](05-treble-wall.md) | Partitions, VINTF, SELinux — where code is allowed to live |
| 6 | [Dynamic availability](06-dynamic-availability.md) | Making the camera appear and vanish with its producer |
| 7 | [A frozen AIDL boundary](07-frozen-boundary.md) | Stable, versioned interfaces and why they enable updates |
| 8 | [Shipping as a vendor APEX](08-vendor-apex.md) | Updatable system components — the whole HAL in one signed file |
| 9 | [GPU compositing, zero conversion](09-gpu-zero-conversion.md) | EGL, EGLImage, FBO/renderbuffer, the all-GPU frame path |

## The through-line

Keep this picture in mind; every lesson fills in one more piece of it.

```
 producer app                         system_server                 vendor APEX
 ┌──────────────┐  registerCamera()   ┌───────────────────┐         ┌───────────┐
 │ draws with   │────────────────────▶│ VirtualCamera     │         │  camera   │
 │ OpenGL ES    │◀── Surface ─────────│ Service + BufferQ │         │  HAL      │
 └──────┬───────┘   (producer end)    └─────────┬─────────┘         └─────┬─────┘
        │ queue frames (GPU)                    │ frame handles           │
        └───────────────────────────────────────▶ ── frozen AIDL ────────▶│
                                                                   GPU blit│
                                                                           ▼
                                            cameraserver ◀── camera 100 ── fill
                                                 │
                                                 ▼
                                            any camera app's preview
```

Lesson 1 starts at the far right (the HAL that makes a camera exist) and each
lesson works outward until the whole loop is closed.

## How to follow along

There are two reference targets, with different qualification boundaries:

- **Android 13 Cuttlefish:** root `platform/`, `service/`, `platform-jni/`,
  `hal/aidl-v1/` and the [A13 integration script](../../scripts/integrate-a13-platform.sh).
  Earlier matched A13 cohorts have Enforcing and staged-APEX evidence; those
  receipts do not automatically qualify every newer source revision.
- **Android 17 Raspberry Pi 5:** the [target guide](../../targets/rpi5-a17/README.md),
  pinned manifest and target integrator. Its producer app is in
  `targets/rpi5-a17/apps/VCamProducer`, not the root A13 app directory.
  The tested Pi base is userdebug/test-signed and SELinux permissive.

Examples are conceptual unless explicitly identified as source excerpts.
`SERIAL`, paths and output counts are placeholders, not commands to run blindly
against whichever device happens to be connected. Read [lesson 8](08-vendor-apex.md)
before building or updating. This review did not rebuild or requalify either target.

The [latest Pi investigation](../pi5-extensive-20261024/README.md) distinguishes
successful capture callbacks, independently decoded pixels and presentation.
A 120-fps request or callback rate is not proof of 120 distinct displayed frames.

A recurring feature: **"What broke for real."** Each lesson ends with the actual
bugs hit while building this — because the failure modes teach the concept as
sharply as the happy path.

Start with [Lesson 1 →](01-camera-as-a-service.md)
