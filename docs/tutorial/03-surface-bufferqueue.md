# Lesson 3 — Surface & BufferQueue

**What you'll understand:** the single producer/consumer primitive that every
frame in Android rides on — every preview, every video, every window on screen —
and why a `Surface` is really just one end of it.

---

## The need

Lesson 2 gave us a buffer and a way to fill it. But a virtual camera's content
comes from *another app*. So we need a way for one process to **draw** into
buffers that another process will **read** — without copying, and without the
two sides stepping on each other (reading a half-drawn frame, or overwriting a
frame still being read).

Android's answer is one small, ubiquitous data structure.

## BufferQueue: a conveyor belt of buffers

A **BufferQueue** is a ring of a few graphics buffers (typically 3–4) with a
**producer** end and a **consumer** end. It choreographs ownership so exactly
one side touches a given buffer at a time:

```
 producer (drawing)                          consumer (reading)
   dequeueBuffer() ─▶ [ B0 ][ B1 ][ B2 ] ─▶ acquireBuffer()
        write                                     read
   queueBuffer()   ◀─    recycled           ◀─ releaseBuffer()
```

- The producer **dequeues** a free buffer, draws into it, and **queues** it.
- The consumer **acquires** the most recent queued buffer, reads it, and
  **releases** it back to the free pool.

Buffers cycle; ownership is explicit; **no pixels are ever copied** — only the
handle's ownership moves around the ring. This one structure is the backbone of
Android graphics: SurfaceFlinger composites windows this way, the video encoder
consumes frames this way, and — as we'll build — a camera can too.

## A Surface *is* the producer end

Here's the reframing that makes the rest of the system click:

> A **`Surface`** is simply the **producer end of a BufferQueue**, wrapped as an
> object you can hand to another process over Binder.

When an API gives you a `Surface` to "draw into" — a `SurfaceView`, a
`MediaCodec` input, a camera preview target — it is handing you permission to
`dequeue → draw → queue` buffers that *someone else* owns and consumes. You draw;
they read; the buffers never move.

That's why the whole virtual-camera design is possible: if the *camera side*
owns a BufferQueue and hands its **Surface** to a producer app, the app can draw
frames that the camera consumes — cross-process, zero-copy — using nothing more
exotic than "draw into this Surface."

## Where this lives in the code

In the shipping design (later lessons refine *where* it runs), the BufferQueue
is created on the platform side and its producer end is exported as a Surface for
the app. You can see the raw creation in the platform frame pump
[`platform-jni/VirtualCameraRelayJni.cpp`](../../platform-jni/VirtualCameraRelayJni.cpp):

```cpp
// Create the queue; wrap the consumer end; export the producer end as a Surface.
sp<IGraphicBufferProducer> producer;
sp<IGraphicBufferConsumer> consumer;
BufferQueue::createBufferQueue(&producer, &consumer);

sp<BufferItemConsumer> bic = new BufferItemConsumer(
        consumer,
        GRALLOC_USAGE_HW_TEXTURE,        // ← the consumer's usage (see below)
        /*maxAcquiredBuffers*/ 2, false);
bic->setDefaultBufferSize(width, height);
bic->setDefaultBufferFormat(PIXEL_FORMAT_RGBA_8888);
bic->setFrameAvailableListener(gListener);   // "a new frame arrived" callback

return android_view_Surface_createFromIGraphicBufferProducer(env, producer);
// ↑ the Java Surface handed to the producer app
```

The consumer side registers a **frame-available listener**. Each time the
producer queues a frame, the listener fires, the consumer **acquires** it, does
something with the handle, and **releases** it:

```cpp
void onFrameAvailable(const BufferItem&) override {
    BufferItem item;
    consumer->acquireBuffer(&item, 0);
    sp<GraphicBuffer> gb = item.mGraphicBuffer;   // the frame, by handle
    // ... forward/consume the handle (no pixel copy) ...
    consumer->releaseBuffer(item);
}
```

## The usage rule returns

Look again at that `GRALLOC_USAGE_HW_TEXTURE` on the consumer. Remember Lesson
2's rule: *the buffer's usage is the union of everyone's needs.* The producer end
adds its own flags when it dequeues (a GL producer adds "GPU render target"). The
buffer gralloc finally allocates satisfies **both** — GPU-renderable by the
producer, GPU-sampleable by the consumer.

Get this union wrong and you get the Lesson 2 failure again. In fact the value
here changed over the project's life:

- Early on the consumer asked for `SW_READ | SW_WRITE` (the HAL read frames on
  the CPU, and a *software-canvas* producer wrote on the CPU).
- Once both producer and consumer moved to the GPU (Lesson 9), the right usage
  became `HW_TEXTURE` — GPU-optimal, no CPU-mappable layout forced.

The BufferQueue didn't change; only the declared usage did. That's the
primitive's flexibility: the same conveyor serves a CPU pipeline or a GPU one,
depending on what the two ends declare they'll do.

## What broke for real

* **The producer couldn't lock the Surface.** With the consumer declaring only
  `SW_READ`, a `Surface.lockCanvas()` on the producer (a CPU write) threw
  `IllegalArgumentException` — the allocated buffer had no CPU-writable layout.
  The union of {consumer reads on CPU} ∪ {producer writes on CPU} needs *both*
  SW flags. Adding `SW_WRITE_OFTEN` fixed it. (Same root cause as Lesson 2's
  second note — it bites at every queue boundary until you internalize it.)

## Takeaways

- A **BufferQueue** is a small ring of buffers with a producer and consumer end;
  it choreographs ownership so pixels are never copied and never raced.
- A **`Surface` is the producer end** of a BufferQueue, portable across
  processes — "draw into this Surface" means "produce into my queue."
- A virtual camera works by **owning the queue and handing its Surface to the
  app**: the app produces, the camera consumes.
- The **usage-union rule** governs whether the two ends can actually touch the
  buffers the way they intend.

We can now move a frame from one app to another. But how does the producer app
*find* the camera and *get* that Surface in the first place? That needs a control
channel — and Android's control channel is Binder.

[← Lesson 2](02-gralloc-buffers.md) · [Lesson 4: Binder & a control plane →](04-binder-control-plane.md)
