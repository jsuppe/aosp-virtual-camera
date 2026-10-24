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
doesn't — not from the app. In this project's AIDL camera path there are
**two Binder process crossings** in the simplified diagram:

```
 your app  ──Binder──▶  cameraserver  ──Binder/AIDL──▶  a camera HAL  ──▶ hardware
 (Camera2)              (the broker)                    (the "driver")
```

* **cameraserver** is a system process — the camera broker. It normally brokers app access to camera HALs and arbitrates client requests.
  Test tools such as VTS can also call a provider directly. Ordinary Camera2
  apps do not call this HAL directly.
* A **camera HAL** (Hardware Abstraction Layer) is the vendor-supplied piece
  that implements device behavior. A provider can expose multiple cameras;
  there is no required one-provider-per-chip-vendor mapping.

Here is the leverage the whole project rests on: **cameraserver talks to a HAL
through a standard AIDL interface rather than requiring a physical sensor.** Implement that
interface in software and compatible, authorized Camera2 consumers can use it
without a custom consumer SDK. Apps can still filter cameras or request
unsupported capabilities; universal app compatibility is not implied.

## What a HAL actually is

In this project, the binderized AIDL camera HAL is **a native service that
registers a well-known interface with `servicemanager`.** Other HAL generations
can use different mechanisms, including HIDL. Here the provider interface is:

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

The second surprise: **HAL capture results answer capture requests**. Camera2
has asynchronous result and image callbacks, but these are driven by requests,
including repeating requests, rather than an unsolicited HAL image feed:

1. cameraserver hands the HAL a **capture request**. Crucially, that request
   contains an **empty output buffer** — memory the framework already allocated.
2. The HAL's job is to **fill that buffer** and hand it back.
3. Repeat, ~30 times a second. The app's "preview" is just this stream of
   filled buffers being displayed.

You can see the shape of it in the session code — the loop that answers each
request lives in
[`hal/aidl-v1/VirtualCameraSession.cpp`](../../hal/aidl-v1/VirtualCameraSession.cpp),
starting in `processCaptureRequest` and its capture-processing helpers.
Conceptual pseudocode, omitting validation, fences, metadata and errors:

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

As a hypothetical first implementation (not the current no-source policy),
notice what a *minimal* virtual camera would need: it doesn't have to render
anything interesting. If it just returned every buffer with `status = OK` and
solid gray pixels, camera 100 would already work — apps would open it and see
gray. That's the first checkpoint on the arc: **a camera that exists.** Making
those pixels *interesting* is the rest of the tutorial.

## Enumeration: how camera 100 shows up

Before capture, the framework queries the provider and listens for device-status
changes. The current provider enumerates **present slots**, not an unconditional
camera 100:

```cpp
// Current getCameraIdList loop; surrounding null check/logging omitted.
for (int s = 0; s < ::virtualcamera::kMaxVirtualCameras; s++) {
    if (mSlotPresent[s].load(std::memory_order_acquire)) {
        cameraIds->push_back(::virtualcamera::deviceIdForSlot(s));
    }
}
```

[VirtualCameraSlots.h](../../hal/core/VirtualCameraSlots.h) maps slot 0 to
`device@1.0/virtual_renderer/100`, slot 1 to `.../101`, up to slot 3.
The app-facing Camera2 IDs are strings such as `"100"`. These are **not** the
broker registration IDs returned to producers (Lesson 4).

Enumeration alone is not a usable camera: metadata, device/session interfaces,
stream negotiation, shutter/results, buffer ownership and errors must also meet
the camera contract. Inspect the actual state with:

```sh
adb -s SERIAL shell dumpsys media.camera
```

No registered producer normally means none of these virtual IDs is exposed.
Lesson 6 explains publication and removal.

## What broke for real

* **A camera provider that registers but is never *declared* won't be used.**
  `servicemanager` enforces a manifest (VINTF, Lesson 5): if
  the exact `ICameraProvider/virtual_renderer/0` instance isn't declared,
  registration can fail. Inspect the returned status, servicemanager logs and
  SELinux denials rather than assuming a silent or uniquely diagnosed failure.
* **The service is a *service*, not an activity.** Early on the producer app was
  launched with `monkey`, which expects a launchable activity and failed with a
  cryptic exit code. A HAL-adjacent service is started explicitly (via init, or
  `am start-foreground-service`), never "opened."

## Takeaways

- An Android camera is a **service you call across processes**, brokered by
  **cameraserver**, not a device you open.
- This **HAL** is a native service implementing the standard AIDL camera
  contract; consumers do not need a custom API just because its source is software.
- Frames are **pulled**: the framework hands you an empty buffer per request and
  you fill it.
- Enumeration is only the start: publish valid capabilities and return success
  only for genuinely filled buffers, with correct synchronization and metadata.

Next we tackle the thing we hand-waved — *putting pixels into that buffer* — and
discover the type that all of Android graphics is built on.

[← Index](00-index.md) · [Lesson 2: Graphics buffers & gralloc →](02-gralloc-buffers.md)
