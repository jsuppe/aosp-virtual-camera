# Lesson 1 — The camera as a service

**What you'll understand:** that an Android camera is not a device file you open
but a *service you call*, and that "implementing a camera" means implementing a
standard interface that the framework already knows how to talk to.

---

## The surprise

When an app opens a camera, it writes something like:

```java
CameraManager cm = getSystemService(CameraManager.class);
cm.openCamera("100", stateCallback, handler);
```

It's tempting to imagine this reaches down to `/dev/video0` and a driver. It
doesn't — not from the app, not even close. The call crosses **three process
boundaries** before anything hardware-shaped happens:

```
 your app  ──Binder──▶  cameraserver  ──Binder/AIDL──▶  a camera HAL  ──▶ hardware
 (Camera2)              (the broker)                    (the "driver")
```

* **cameraserver** is a system process — the camera broker. It's the only thing
  that talks to camera HALs directly, and it multiplexes every app's requests
  onto them. Your app never touches a HAL.
* A **camera HAL** (Hardware Abstraction Layer) is the vendor-supplied piece
  that actually knows a specific sensor. On a real phone there's one per camera
  chip vendor.

Here is the leverage the whole project rests on: **cameraserver talks to a HAL
through a standard AIDL interface, and it cannot tell whether the thing on the
other end is a driver or a program pretending to be one.** Implement that
interface in software and you have a camera that every unmodified camera app can
open.

## What a HAL actually is

Strip away the acronym and a HAL is just **a long-running native service that
registers a well-known interface with `servicemanager`.** For camera, the
interface is:

```
android.hardware.camera.provider.ICameraProvider
```

Our implementation of it lives in
[`hal/aidl-v1/VirtualCameraProvider.cpp`](../../hal/aidl-v1/VirtualCameraProvider.cpp).
The entry point that registers it is tiny — this is the whole "become a camera
driver" ceremony:

```cpp
// hal/aidl-v1/service.cpp  (abridged)
auto provider = ndk::SharedRefBase::make<VirtualCameraProvider>();
const std::string instance =
        std::string(VirtualCameraProvider::descriptor) + "/virtual_renderer/0";
AServiceManager_addService(provider->asBinder().get(), instance.c_str());
ABinderProcess_joinThreadPool();
```

`AServiceManager_addService` publishes the object under a name. From that moment
cameraserver can discover it and route to it. (There's a gate — the name has to
be *declared* and *allowed*; that's Lessons 4 and 5. For now, the shape is what
matters.)

## The request/response model

The second surprise: **a camera does not push frames.** There is no callback
that fires 30 times a second with a picture. Instead the framework *pulls*:

1. cameraserver hands the HAL a **capture request**. Crucially, that request
   contains an **empty output buffer** — memory the framework already allocated.
2. The HAL's job is to **fill that buffer** and hand it back.
3. Repeat, ~30 times a second. The app's "preview" is just this stream of
   filled buffers being displayed.

You can see the shape of it in the session code — the loop that answers each
request lives in
[`hal/aidl-v1/VirtualCameraSession.cpp`](../../hal/aidl-v1/VirtualCameraSession.cpp),
in `processCaptureRequest`. Reduced to its essence:

```cpp
for (const auto& inBuffer : request.outputBuffers) {
    buffer_handle_t handle = importBuffer(inBuffer);   // the framework's buffer
    // ... put pixels into `handle` ...                // (Lesson 2)
    StreamBuffer out;
    out.streamId = inBuffer.streamId;
    out.bufferId = inBuffer.bufferId;
    out.status   = BufferStatus::OK;                   // "I filled it"
    outputBuffers.push_back(out);
}
mCallback->processCaptureResult(...);                  // hand them back
```

Notice what a *minimal* virtual camera would need: it doesn't have to render
anything interesting. If it just returned every buffer with `status = OK` and
solid gray pixels, camera 100 would already work — apps would open it and see
gray. That's the first checkpoint on the arc: **a camera that exists.** Making
those pixels *interesting* is the rest of the tutorial.

## Enumeration: how camera 100 shows up

Before a capture, an app asks "what cameras exist?" The framework calls the
provider's `getCameraIdList`. Ours returns one id:

```cpp
// hal/aidl-v1/VirtualCameraProvider.cpp
ndk::ScopedAStatus VirtualCameraProvider::getCameraIdList(
        std::vector<std::string>* cameraIds) {
    cameraIds->push_back(kVirtualCameraId);  // "device@1.0/virtual_renderer/100"
    return ndk::ScopedAStatus::ok();
}
```

That single line is why `100` appears in `cm.getCameraIdList()`. Later
(Lesson 6) we'll make this list *conditional* — the camera should only exist
while a producer is feeding it — but the mechanism is this one method.

You can watch it on a running device:

```
$ adb shell dumpsys media.camera | grep "maps to"
    Device 0 maps to "0"
    Device 1 maps to "1"
    Device 2 maps to "100"     ← our virtual camera
```

## What broke for real

* **A camera provider that registers but is never *declared* won't be used.**
  `servicemanager` enforces a manifest (VINTF, Lesson 5): if
  `ICameraProvider/virtual_renderer` isn't listed, registration is silently
  refused and camera 100 never appears — with no obvious error at the app.
* **The service is a *service*, not an activity.** Early on the producer app was
  launched with `monkey`, which expects a launchable activity and failed with a
  cryptic exit code. A HAL-adjacent service is started explicitly (via init, or
  `am start-foreground-service`), never "opened."

## Takeaways

- An Android camera is a **service you call across processes**, brokered by
  **cameraserver**, not a device you open.
- A **HAL** is a native service implementing a standard AIDL interface; software
  can implement it and cameraserver can't tell the difference.
- Frames are **pulled**: the framework hands you an empty buffer per request and
  you fill it.
- Making the camera *exist* is one method (`getCameraIdList`) plus answering
  requests with `status = OK`.

Next we tackle the thing we hand-waved — *putting pixels into that buffer* — and
discover the type that all of Android graphics is built on.

[← Index](00-index.md) · [Lesson 2: Graphics buffers & gralloc →](02-gralloc-buffers.md)
