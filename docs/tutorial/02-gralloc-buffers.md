# Lesson 2 — Graphics buffers & gralloc

**What you'll understand:** what the "empty buffer" from Lesson 1 actually is,
why passing its handle avoids copying the pixels, and how allocation/import
errors can break the pipeline.

---

## The problem that shapes everything

A 4K RGBA frame is 3840 × 2160 × 4 bytes ≈ **33 MB**. At 30 fps that's **a
gigabyte per second**. Two consequences fall out immediately and they explain
almost every design decision in Android graphics:

1. You cannot *copy* frames casually. A single memcpy of a 4K frame is already
   expensive; doing it per stage per frame is ruinous.
2. You *definitely* cannot send frames *through* IPC — serializing 33 MB over
   Binder 30 times a second is absurd.

So Android almost never moves pixels. It moves **handles to pixels.**

## The graphics buffer

The unit of currency is the **graphics buffer**. You'll meet it under three
names — related representations/wrappers of the same underlying allocation,
not interchangeable C/C++ types:

| Name | Where you see it |
|------|------------------|
| `buffer_handle_t` (a `native_handle_t*`) | C HAL code — a bundle of file descriptors + ints |
| `AHardwareBuffer` | NDK / modern native code |
| `GraphicBuffer` | libui / platform C++ |

It's allocated by **gralloc** ("graphics alloc"), the memory allocator HAL. On
Cuttlefish the gralloc implementation is **minigbm**. The buffer is real memory
(often in a special region the GPU/display can reach), and its defining property
is:

> A graphics buffer is shared across processes **by handle** — a set of file
> descriptors — not by copying its contents.

Passing a handle avoids serializing those 33 MB. IPC, fd duplication, import
and synchronization still cost time; no universal microsecond bound is implied.
**This is zero-copy transport**, not proof that later rendering performs no copies.
The framework's "empty output buffer" from Lesson 1 is exactly this: a handle to memory the framework
allocated, handed to your HAL so you can fill the memory in place.

## Usage flags: the buffer's contract

When gralloc allocates a buffer, the caller declares **usage flags** — who will
touch this buffer and how:

```
GRALLOC_USAGE_HW_TEXTURE     the GPU will sample it
GPU_RENDER_TARGET            the GPU will draw into it
GRALLOC_USAGE_SW_READ_OFTEN  the CPU will map and read it
GRALLOC_USAGE_SW_WRITE_OFTEN the CPU will map and write it
CAMERA_OUTPUT                it's a camera stream buffer
```

The allocator uses these to choose a **memory type and pixel layout** that
satisfies *everyone* who will touch the buffer. This matters more than it
looks:

> A buffer's usage is the **union of every accessor's needs.** Get it wrong and
> a consumer or producer literally cannot map the buffer.

We hit exactly this (see "What broke", below). Keep the rule in mind — it
returns in Lesson 3 and Lesson 9.

## Importing: you can't just use a handle

Here's the subtlety that catches everyone. When your process *receives* a
`buffer_handle_t` from another process, you cannot dereference it and start
writing. The handle's file descriptors are valid in your process, but the
buffer has not necessarily been imported into the local mapper/graphics
context. Import is distinct from CPU mapping: CPU access additionally requires
a supported lock/map operation and the proper synchronization.

You must **import** it through the **gralloc mapper**:

```cpp
// hal/aidl-v1/VirtualCameraSession.cpp — importBuffer()
if (!sHandleImporter.importBuffer(bufHandle)) {
    ALOGE("Failed to import buffer");
    return nullptr;
}
```

`sHandleImporter` is a small helper around the mapper. Import validates the
handle and makes the buffer usable locally. Only *after* a successful import can
you lock it and write pixels:

```cpp
// Conceptual CPU-path sketch; see FrameFiller.cpp for actual helper signatures.
auto dst = lockYCbCrCompat(importer, handle, usage, width, height);
// dst.y, dst.cb, dst.cr now point at mappable planes → write them
```

> **Never return success for an unfilled buffer.** Earlier prototypes could
> look like successful black frames. The current session reports buffer/request
> errors when import or filling fails; a black preview by itself is not a diagnosis.

## Filling a frame (the CPU way)

The oldest, simplest fill path in this repo is
[`core/FrameFiller.cpp`](../../hal/core/FrameFiller.cpp). It locks the output buffer
and writes pixels with the CPU. Because camera streams are often YUV and app
content is usually RGBA, it converts per pixel:

```cpp
// core/FrameFiller.cpp — the per-pixel RGBA→YUV conversion (BT.601)
static inline void rgbaToYuv(uint8_t r, uint8_t g, uint8_t b,
                             uint8_t* y, uint8_t* cb, uint8_t* cr) {
    *y  = ((66*r + 129*g + 25*b + 128) >> 8) + 16;
    *cb = ((-38*r - 74*g + 112*b + 128) >> 8) + 128;
    *cr = ((112*r - 94*g - 18*b + 128) >> 8) + 128;
}
```

This works and it's a fine place to start — but notice the cost: it's a full CPU
pass over every pixel, ~8.3 million pixels per 4K frame, each requiring multiple operations. Hold that thought;
**Lesson 9 removes CPU pixel conversion for the RGBA fast path** and explains
GPU-assisted YUV and CPU JPEG paths. For now, the
point is that once a buffer is imported and locked, filling it is "just" writing
memory.

`FrameFiller` retains synthetic color-bar helpers from earlier designs. They
are **not the current stable-AIDL no-source behavior**: without a registered
producer the device is absent, and without a fresh, ready source the capture
request fails instead of returning synthetic or cached success.

## What broke for real

* **`Driver is uninitialized`.** The minigbm mapper initializes its DRM device
  **once, in its constructor, at first use.** In an early design the HAL ran in
  a process that was denied `/dev/dri` by SELinux at that first moment — the
  mapper died and *every* later import failed with
  `Failed to import buffer. Driver is uninitialized`. Relaxing the policy
  afterward didn't help — the constructor had already run. The fix was to
  restart the HAL process after fixing access (and, ultimately, to run it in a
  domain that has the access from the start — Lesson 5). *Lesson: some init is
  once-per-process; a late permission grant doesn't undo a poisoned singleton.*
* **`lockCanvas` threw `IllegalArgumentException`.** A producer drew into a
  buffer with a software canvas (a CPU *write*), but the buffer had been
  allocated with only `SW_READ` usage. The allocator gave a layout the CPU
  couldn't write. Adding `SW_WRITE_OFTEN` fixed it. *Lesson: the usage-is-a-union
  rule is not academic.*

## Takeaways

- Android moves **handles, not pixels**; a graphics buffer is shared
  cross-process by file descriptors. That's "zero-copy."
- **Usage flags** are a contract; a buffer's usage must cover *every* accessor
  or someone can't map it.
- A received handle must be **imported** through the gralloc mapper before use;
  failed import must produce an error, not a fabricated successful frame.
- Filling on the CPU means lock → write → (often) color-convert — correct but a
  full-frame CPU cost avoided by the RGBA GPU path, not by every output path.

We can now fill a frame. But a virtual camera's whole point is to show *another
app's* content. Next: the mechanism that lets one app draw into a buffer another
process consumes.

[← Lesson 1](01-camera-as-a-service.md) · [Lesson 3: Surface & BufferQueue →](03-surface-bufferqueue.md)
