# Lesson 4 — Binder & a control plane

**What you'll understand:** how the producer app finds the camera and receives a
Surface — Android's IPC (Binder/AIDL), how services are found by name, and how to
notice when the other side dies.

---

## Two planes: data and control

Lesson 3 gave us the **data plane** — frames flowing through a BufferQueue. But
something has to set that up: the producer app must *find* the virtual camera,
*announce* "I want to be its content source," and *receive* the Surface to draw
into. Frames don't carry any of that. We need a **control plane**, and in Android
the control plane is **Binder**.

Two planes, different jobs:

| Plane | Carries | Mechanism |
|-------|---------|-----------|
| Data | frames (33 MB each) | BufferQueue — handles, zero-copy |
| Control | "register me", "here's your Surface", "camera closed" | Binder/AIDL — small messages |

Keeping them separate is deliberate: you never want a 33 MB frame going through
Binder. Frames ride the queue; only *tiny* messages and the *handle* to the
Surface cross Binder.

## Binder and AIDL in one paragraph

**Binder** is Android's inter-process call mechanism — think "typed, secure RPC
between processes." You describe an interface in an **`.aidl`** file; the build
generates client and server stubs in Java and/or C++. A server **registers** its
object with **`servicemanager`** under a string name; a client **looks it up** by
that name and calls methods as if local. Binder also carries **object
references** — you can pass a `Surface` (which wraps a BufferQueue producer) or a
callback interface across the wire, and the other side can call back.

## The virtual camera's control interface

The producer talks to a system service through a small AIDL interface,
`IVirtualCameraService`. The two calls that matter:

```aidl
interface IVirtualCameraService {
    int  registerCamera(in VirtualCameraConfig config, IVirtualCameraCallback cb);
    void unregisterCamera(int cameraId);
}
```

- `registerCamera` says "I'm a producer; here's my config, and here's a callback
  you can reach me on."
- `IVirtualCameraCallback` is the *reverse* direction — the service calls the app
  when there's a Surface to hand over:

```aidl
interface IVirtualCameraCallback {
    void onStreamsConfigured(in StreamConfig[] streams, in Surface[] surfaces);
    void onCameraClosed();
}
```

That `Surface[]` in `onStreamsConfigured` is the whole point: it's how the
producer-end of the camera's BufferQueue (Lesson 3) reaches the app. Binder
carried the Surface object across the process boundary.

## Registration, in code

The producer app —
[`platform/apps/VCamProducer/.../VCamProducerService.java`](../../platform/apps/VCamProducer/src/com/example/vcamproducer/VCamProducerService.java) —
looks the service up by name and registers:

```java
IBinder binder = ServiceManager.getService("virtual_camera");
mService = IVirtualCameraService.Stub.asInterface(binder);

VirtualCameraConfig config = new VirtualCameraConfig();
config.name = "VCamProducer";
config.maxWidth = 1920; config.maxHeight = 1080; config.maxFps = 30;
mCameraId = mService.registerCamera(config, mCallback);   // ← mCallback is the reverse channel
```

And the service —
[`service/VirtualCameraService.java`](../../service/VirtualCameraService.java) —
records the producer and, when the camera's streams are configured, calls back
with the Surface:

```java
public int registerCamera(VirtualCameraConfig config, IVirtualCameraCallback cb) {
    final int id = mNextCameraId.getAndIncrement();
    // ... store {id, config, cb} ...
    return id;
}
```

When a consumer later opens the camera and streams get configured, the service
invokes `cb.onStreamsConfigured(streams, surfaces)` — and the app's render
thread (Lesson 3 / Lesson 9) starts drawing into `surfaces[0]`.

## Death: the part beginners skip

Distributed systems fail, and a producer app can crash or be killed at any
moment. If the service kept handing out a dead producer's Surface, the camera
would freeze or worse. Binder has a built-in answer: **`linkToDeath`**. You
register a recipient and the kernel notifies you when the other end's process
dies.

```java
// service/VirtualCameraService.java
callback.asBinder().linkToDeath(() -> {
    Log.w(TAG, "Renderer for camera " + id + " died, unregistering");
    removeCamera(id);           // clean up as if it unregistered
}, 0);
```

This one hook is what makes "the camera disappears when the producer dies"
robust rather than hopeful (we lean on it hard in Lesson 6). Treat death
notification as part of the happy path, not an afterthought — every Binder
relationship that owns a resource should have one.

## What broke for real

* **Ordering assumptions.** Early demos required starting the producer *before*
  the viewer, because registration and stream-config were racing. The durable
  fix (Lesson 6) was to make the camera's very existence track producer
  registration, so start order stops mattering — but the first symptom was "works
  if I launch them in the right order," which is always a smell that a control-
  plane event is missing.
* **A service is not launchable.** `IVirtualCameraService` is hosted in
  system_server; the *producer* is a foreground `Service` with no activity. You
  start it with `am start-foreground-service`, and it must hold `FOREGROUND_SERVICE`
  + be granted `CAMERA`. Trying to "open" it like an app fails.

## Takeaways

- Split **data** (frames, via BufferQueue) from **control** (setup/teardown, via
  Binder). Never send frames through Binder.
- **AIDL** interfaces + **servicemanager** registration/lookup are how the
  producer finds the camera; Binder can carry a **`Surface`** and a **callback**
  across the boundary.
- **`linkToDeath`** turns "the producer vanished" from a hang into a clean
  teardown — build it in from the start.

We now have a producer registering with a service and receiving a Surface. But
*where do these pieces run*, and why can't they all just live together? That's
the Treble wall — the constraint that shapes the entire final architecture.

[← Lesson 3](03-surface-bufferqueue.md) · [Lesson 5: The Treble wall →](05-treble-wall.md)
