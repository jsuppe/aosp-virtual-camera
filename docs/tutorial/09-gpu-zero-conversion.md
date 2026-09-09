# Lesson 9 — GPU compositing, zero conversion

**What you'll understand:** how to render the producer's frames on the GPU and
fill the camera buffer with **no color conversion and no CPU touch** — and the GL
concepts (EGLImage, texture vs renderbuffer, FBO) that make it work. This is the
lesson that ties the whole arc together.

---

## The one remaining CPU cost

Every lesson so far left one thing on the CPU: the **fill**. Lesson 2's
`FrameFiller` locks the camera output buffer and writes it pixel-by-pixel,
converting RGBA→YUV — ~8 million operations per 4K frame, the single biggest cost
in the pipeline. Let's remove it.

First, two facts, stated precisely — because "zero-copy" gets overclaimed here:

- **The copy is unavoidable.** The camera framework *owns* the output buffer
  (it's bound to the consumer app's Surface, Lesson 3). The producer drew into a
  *different* buffer. Two buffers, two queues → the HAL must move pixels from one
  to the other. No producer≠consumer topology escapes this move.
- **The color conversion is avoidable.** *Moving* pixels and *converting* them
  are different costs. If both buffers are RGBA, moving is a plain blit with
  nothing to convert.

So the honest target is **one GPU blit, no conversion, no CPU touch** — not "zero
copy." Here's how each piece gets there.

## Step 1 — the producer renders on the GPU

The producer stops using a software canvas and renders with **OpenGL ES**. It
wraps the Surface (an `ANativeWindow`) in an `EGLSurface` and draws a fragment
shader — see
[`VCamProducerService.java`](../../platform/apps/VCamProducer/src/com/example/vcamproducer/VCamProducerService.java):

```java
EGLSurface win = EGL14.eglCreateWindowSurface(dpy, cfg, mSurface, …);
EGL14.eglMakeCurrent(dpy, win, win, ctx);
// … a full-screen shader draws a time-varying gradient + a moving disc …
GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
EGL14.eglSwapBuffers(dpy, win);        // queues a GPU-rendered RGBA buffer
```

Because EGL allocates the Surface's buffers with GPU render-target usage
(remember the usage-union rule, Lessons 2–3), **no CPU touches the producer's
pixels.** The frames it queues are RGBA in GPU memory.

## Step 2 — three GL concepts for the fill

The HAL now needs to move that GPU frame into the camera's output buffer on the
GPU. Three concepts, each an analogue of something you already know.

**EGLImage — the GPU's cross-process handle.** Lesson 2 said a gralloc buffer is
shared by handle. An **`EGLImage`** is the GPU-side equivalent: a lightweight
wrapper that lets a GL context *point at* an existing gralloc buffer without
copying. You build one from an `AHardwareBuffer` and bind it to a GL object. This
is "handles travel, pixels don't" — for the GPU.

**Texture vs renderbuffer — read vs write.** A GL object backed by an EGLImage
is one of two things:
- a **texture**, which a shader **samples** (reads) — this takes the producer
  frame as *input*;
- a **renderbuffer**, which the GPU **draws into** (writes) — this targets the
  camera output buffer as *output*.

The trap: you *cannot* attach an imported-gralloc **texture** as a render target
on most drivers — the framebuffer comes back "incomplete." Imported buffers are
only reliably renderable through a **renderbuffer**
(`glEGLImageTargetRenderbufferStorageOES`).

**FBO — an off-screen draw target.** A **Framebuffer Object** redirects GL
drawing away from the screen to a buffer you attach — here, the renderbuffer
wrapping the camera output. Attach it, draw a full-screen quad whose shader
samples the source texture, and the result lands in the output buffer.

## Step 3 — the compositor

Put together in
[`core/GpuCompositor.cpp`](../../hal/core/GpuCompositor.cpp), the per-request move is:

```
producer AHB ──EGLImage──▶ GL texture (sampled) ─┐
                                                  ├─ full-screen shader ──▶
output buffer ──EGLImage──▶ renderbuffer ─FBO────┘   (RGBA→RGBA: a copy,
                                                       never a conversion)
```

```cpp
// import source as a sampled texture
glBindTexture(GL_TEXTURE_2D, mSrcTex);
glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, srcImg);
// import destination as a RENDERBUFFER (renderable; texture attach = incomplete)
glEGLImageTargetRenderbufferStorageOES_(GL_RENDERBUFFER, dstImg);
glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_RENDERBUFFER, dstRb);
// one passthrough blit
glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
```

## Step 4 — the format nudge (why it's *zero* conversion)

The blit only converts if the two formats differ. A preview usually asks for
`IMPLEMENTATION_DEFINED` — "HAL, you pick." So the HAL **picks RGBA**, in
[`VirtualCameraSession.cpp`](../../hal/aidl-v1/VirtualCameraSession.cpp)'s
`configureStreams`:

```cpp
if (resolvedFormat == kImplDefined) resolvedFormat = kRGBA8888;   // pick RGBA
// … and request GPU_RENDER_TARGET buffers for the RGBA path …
```

Now source is RGBA, destination is RGBA, and the shader pass is a straight
passthrough — **zero color conversion.** A `SurfaceTexture` consumer (our viewer)
samples RGBA fine, so the common preview case lands entirely on this path. A
consumer that *hard-requires* YUV falls back to the CPU `FrameFiller` — labeled,
honest, and the only path that still costs a CPU pass (moving it to the GPU is
future work).

## The bonus: correctness, not just speed

Recall Lesson 6's "keep the newest frame" store. With the old *software*-canvas
producer that was safe only because CPU drawing is synchronous. A **GPU** producer
finishes asynchronously — naively reading its buffer could catch a half-drawn
frame. And the reverse hazard exists too: once the platform hands a buffer back
to the BufferQueue, the producer may start drawing into it while the HAL's GPU is
still reading it.

The first GPU version leaned on the driver's pipeline ordering for the first
hazard and hid the second behind a `glFinish()`. The finished design makes both
explicit with **sync fences**, the same mechanism SurfaceFlinger and every
camera HAL use:

- The producer's *acquire fence* travels down with the handle in
  `queueFrameFenced` (a V2 method — see Lesson 7's postscript). The HAL's GPU
  waits on it with `eglWaitSyncKHR`; no CPU ever blocks.
- The compositor's blit produces a *native fence* (`EGL_ANDROID_native_fence_sync`
  → one fd). It becomes the camera framework's release fence for the output
  buffer, **and** it goes back up to the platform when the frame is retired, so
  the platform releases the source buffer *with* it and the producer's next
  `dequeueBuffer` waits for the HAL's read.
- The request loop is paced to `AE_TARGET_FPS_RANGE` (30 fps by default)
  instead of free-running, and reports the paced slot as the sensor timestamp.

Correctness by construction, and one fewer `glFinish()` on a binder thread.

## Watch it

```
$ adb logcat | grep -E "VCamGpuCompositor|PRODUCED .*GL|RECEIVED"
  VCamGpuCompositor: GPU compositor initialized (…NVIDIA GeForce RTX 3090…)
  VCamProducer: PRODUCED 300 frames (GL)
  VCamViewer:   RECEIVED 300 frames from camera 100
```

Steady state: **0 GPU-composite errors, 0 CPU-fallback calls** — no pixel touches
the CPU, end to end.

## What broke for real

* **`FBO incomplete`.** The first version attached the destination EGLImage as a
  *texture* — `glCheckFramebufferStatus` returned not-complete. Imported gralloc
  buffers are only renderable via a **renderbuffer**. (Step 2's trap, learned the
  hard way.)
* **`status = 0x0`, every GL call a silent no-op.** `glCheckFramebufferStatus`
  returned literally `0` — not a valid enum, meaning **no GL context was
  current.** The HAL processes capture requests on the **binder thread pool**, so
  the thread that *initialized* the context wasn't the one *using* it, and an EGL
  context is current on one thread at a time. Fix: `eglMakeCurrent` to acquire at
  the top of each composite and release at the end, serialized by a mutex.
* **Missing ext headers.** `EGLImageKHR` and the `…KHR`/`…OES` entrypoints live in
  `<EGL/eglext.h>` / `<GLES2/gl2ext.h>` and are loaded via `eglGetProcAddress`,
  not linked directly.

## The whole arc, closed

Trace one frame through everything you've learned:

1. The producer draws it with **OpenGL ES** into a Surface (L1 camera exists, L9
   GPU) — the Surface being the producer end of a **BufferQueue** (L3) it got by
   **registering with a Binder service** (L4).
2. The platform side **consumes** the frame by handle and pushes that handle
   **across the Treble wall** (L5) over a **frozen AIDL** interface (L7) —
   `queueFrame(NativeHandle …)`, no pixels through IPC.
3. The **vendor HAL**, shipped as an updatable **APEX** (L8), imports the handle
   and — because a producer is registered, so the camera **exists** (L6) —
   composites it on the **GPU with zero conversion** (L9) into the camera output
   buffer, which is a **gralloc buffer** the framework owns (L2).
4. **cameraserver** hands that buffer to the app's preview (L1). Pixels drawn by
   one app appear in another, across the security boundary, out of a
   hot-swappable component, without ever touching the CPU.

That's the whole system — and now every piece of it should read like something
you understand rather than magic.

[← Lesson 8](08-vendor-apex.md) · [Back to the index](00-index.md) · [Top-level README](../../README.md)
