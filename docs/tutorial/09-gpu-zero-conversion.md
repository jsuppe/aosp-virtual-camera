# Lesson 9 — GPU compositing, zero conversion

**What you'll understand:** the RGBA GPU pixel path, the separate YUV/JPEG paths,
and why GPU rendering does not mean zero copies, waits or CPU work.

## State the claim precisely

In this implementation the producer draws into a source BufferQueue, while
Camera2 supplies different destination buffers. The HAL must fill those output
buffers. For an RGBA source and RGBA destination, it can do a **GPU shader blit
without CPU pixel copying or RGBA→YUV conversion**.

That is not zero-copy rendering: the GPU reads one allocation and writes another.
It is also not a universal requirement for every conceivable camera architecture;
it follows from this implementation's distinct source and destination queues.
Scaling and memory-layout changes can still cost GPU time even when formats match.

## Step 1 — render into the producer Surface

The [A13 producer](../../platform/apps/VCamProducer/src/com/example/vcamproducer/VCamProducerService.java)
uses EGL/OpenGL ES. Conceptually:

```java
// Pseudocode: EGL setup and error handling omitted.
EGLSurface win = EGL14.eglCreateWindowSurface(dpy, cfg, surface, attributes, 0);
EGL14.eglMakeCurrent(dpy, win, win, ctx);
GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
EGL14.eglSwapBuffers(dpy, win);
```

Swap queues a buffer with a producer-completion dependency; it need not mean the
GPU has finished drawing. The Pi implementation is in its
[target-specific producer](../../targets/rpi5-a17/apps/VCamProducer/src/com/example/vcamproducer/VCamProducerService.java).
It includes a CPU-drawn/uploaded clock texture, so calling the entire demo
“no CPU touch” would be wrong even if the HAL's RGBA fill is GPU-only.

## Step 2 — import and draw

[GpuCompositor.cpp](../../hal/core/GpuCompositor.cpp) imports buffers as EGLImages.
An **EGLImage** is a local graphics API object referring to imported storage,
not an object/handle sent across Binder. The gralloc/native handle crosses the
process boundary; each process establishes its own graphics objects.

```text
source AHardwareBuffer → EGLImage → sampled GL texture
                                         ↓ shader quad
output gralloc buffer → EGLImage → renderbuffer attached to FBO
```

A **Framebuffer Object (FBO)** selects the destination for rendering. This
compositor binds the output EGLImage as a renderbuffer and samples the source
texture. The source and destination allocations remain distinct.

The renderbuffer path fixed an incomplete-FBO failure on the tested stack.
That does **not** mean imported textures can never be framebuffer attachments:
support depends on the driver, format, texture target and EGL/GL extensions.
Check framebuffer completeness and errors on the actual target.

The compositor serializes use of its EGL context and makes it current on the
thread performing the operation. A context initialized on one Binder thread
is not automatically current on all threads in the pool.

## Step 3 — choose the output path

[VirtualCameraSession.cpp](../../hal/aidl-v1/VirtualCameraSession.cpp) resolves
`IMPLEMENTATION_DEFINED` to RGBA_8888 and requests GPU-render-target usage for
that stream. The negotiated producer capabilities must allow the configuration.

The current fill paths are:

| Output | Implementation |
|--------|----------------|
| RGBA/RGBX GPU-compatible output | EGLImage import and shader blit; completion fence, or synchronous fallback |
| YUV_420_888 | GPU BT.601 limited-range conversion into packed intermediate planes, then CPU row copies into mapped destination planes; CPU converter fallback |
| JPEG/BLOB | CPU JPEG encoding and camera JPEG blob trailer |

The YUV path is **already implemented**, not future work. It is GPU-assisted,
not direct rendering into the final YUV allocation and not zero-CPU-copy.
`compositeToYcbcr` calls `glFinish` before reading back/copying its intermediate
planes. [FrameFiller.cpp](../../hal/core/FrameFiller.cpp) provides CPU conversion;
[JpegEncoder.cpp](../../hal/core/JpegEncoder.cpp) handles JPEG.
Failed imports, rendering or mapping must report errors, not a successful blank
frame. A fallback only works if the actual allocation supports its access needs.

## Step 4 — preserve fences and freshness

There are three separate synchronization obligations:

1. **Source acquire fence:** do not sample producer pixels before rendering ends.
2. **Destination acquire fence:** do not overwrite a camera buffer before its
   previous user finishes with it.
3. **Completion fences:** do not let the camera consumer read the output, or
   let the producer overwrite the source, before this HAL's GPU work finishes.

The compositor supports importing a source fence for an EGL wait. However,
**the current stable-AIDL capture loop first checks source readiness on the CPU**:
it polls the source fence with cancellable waits of up to 2 ms and a bounded
source-availability/readiness budget. The normal budget is one requested frame
period; the initial attempt after configuration gets at least 200 ms for startup.
Diagnostic-only settings can alter that budget. These are implementation policy,
not end-to-end latency guarantees.

Once the source fence is ready, the session passes no remaining source dependency
to the renderer. Destination-fence waits are also present. Thus the old claim
“no CPU ever blocks” is false even on the RGBA path.

The GPU completion fence protects both destination consumption and source reuse;
the source lease/retirement machinery preserves read dependencies until the JNI
relay can release its held BufferQueue item. If native completion fencing is
unavailable, the compositor can use synchronous `glFinish` for correctness.

Freshness is separate from fence completion. The capture loop requires a newer
source submission sequence for the current configuration, rechecks identity,
and fails on missing, stale, unready or cancelled work. It does not replay its
latest cached frame as a fresh successful result. This admission check still
cannot prove the producer changed its pixels: an independent marker decoder is
needed to measure distinct content.

## What the performance evidence establishes

The [2026-10-24 Pi investigation](../pi5-extensive-20261024/README.md) reports:

- With a temporary clear-before-overwrite candidate and the mirror disabled,
  1080p120 sustained **120 successful Camera2 results/s** for a 60-second window.
- The corresponding full split workload delivered about **12.9 results/s**;
  full 1080p60 delivered 60/s in its measured window.
- An independent non-displaying pixel-decoding workload remained near **60
  distinct frames/s** even when 120 was requested. It did not qualify 120-fps
  fresh-pixel delivery; physical/optical latency was not measured.
- The clear optimization is diagnostic/default-off, not a promoted default.
  The device was restored after testing. Clock-glyph artifacts remain recorded
  as an unresolved producer/demo issue.

These are specific workloads and receipts, not universal Pi limits. Buffer
layout, duplicated rendering, SurfaceView versus TextureView, recording load
and presentation all matter. “No RGBA→YUV conversion” does not mean no bottleneck.

## What broke for real

- Incomplete FBOs required an appropriate renderbuffer import path on the tested
  driver; inspect capabilities instead of generalizing to every GPU.
- GL calls on a thread without the current context did not perform the intended
  operation; make-current/serialization and error checks are necessary.
- Dropping a fence, recycling a held source too early, or accepting an old
  configuration can corrupt frames despite otherwise valid rendering code.

## The whole arc

Producer registration publishes capabilities → Camera2 negotiates output → the
HAL requests a token-bound source configuration → the producer acknowledges and
draws into its Surface → JNI forwards handles/fences → the HAL admits a fresh,
ready source and fills Camera2 buffers → the consumer receives buffers/results.

The APEX packages the vendor half; the integrated platform owns the broker and
queue. Zero-copy **transport**, GPU **fill**, successful **capture**, distinct
**pixel delivery**, and physical **presentation** are different claims.

[← Lesson 8](08-vendor-apex.md) · [Back to the index](00-index.md) · [Top-level README](../../README.md)
