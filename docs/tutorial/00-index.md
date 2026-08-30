# Building a Virtual Camera on Android — A Tutorial

This is a guided tour through the Android graphics and HAL stack, taught by
building one real thing: a **virtual camera** — a camera device (id `100`) that
any normal app can open, whose "sensor" is actually *another app*. By the end
you'll understand how a frame travels from an app's `glDrawArrays` call, across
the platform/vendor security boundary, through an updatable system component,
and out to a camera app's preview — with no color conversion and no CPU touch.

The [top-level README](../../README.md) is the reference explainer for the
*finished* system. This tutorial is the slower path: **one concept per lesson**,
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

The system is validated on **Cuttlefish**, Android's reference virtual device,
running Android 13. Lessons reference `adb` commands and logcat tags so you can
watch each concept work on a running device. You don't need to build anything to
learn — but if you want to, [lesson 8](08-vendor-apex.md) and the
[integration script](../../scripts/integrate-a13-platform.sh) show how the
pieces get into a device image.

A recurring feature: **"What broke for real."** Each lesson ends with the actual
bugs hit while building this — because the failure modes teach the concept as
sharply as the happy path.

Start with [Lesson 1 →](01-camera-as-a-service.md)
