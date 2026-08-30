# Lesson 6 — Dynamic availability

**What you'll understand:** how a camera can *appear* and *disappear* at runtime,
why that makes the system honest ("the camera exists ⇔ a producer is feeding
it"), and the push-based mechanism that keeps the framework in sync.

---

## The behavior we want

So far camera 100 exists always — even with no producer, it shows color bars.
That's fine for a demo, but the *right* semantics are:

> Camera 100 exists **only while a producer app is registered.** Registering adds
> it to the system; unregistering (or the producer dying) removes it.

Why this is worth doing: apps observe camera add/remove through
`CameraManager.AvailabilityCallback`. If the camera's presence tracks the
producer, then **an availability event means something true** — "a virtual camera
is actually producing frames right now," not "a placeholder is installed." It
also makes start-order irrelevant (the Lesson 4 race): a viewer can open the app
first and simply wait for the camera to appear.

This is exactly how **USB webcams** behave — plug one in and cameras appear; that
uses the same platform mechanism we're about to use.

## The mechanism: `cameraDeviceStatusChange`

A camera provider can tell the framework a device came or went:

```
provider ──▶ cameraserver :  cameraDeviceStatusChange(cameraId, PRESENT)
provider ──▶ cameraserver :  cameraDeviceStatusChange(cameraId, NOT_PRESENT)
```

cameraserver then adds/removes the device and fires `onCameraAvailable` /
`onCameraUnavailable` to every app. Two obligations come with using it:

1. `getCameraIdList` (Lesson 1) must **agree** with the pushed state — return the
   id only when present.
2. The provider must actually *send* the transitions at the right moments.

## Wiring it up

**Enumeration becomes conditional.** In
[`VirtualCameraProvider.cpp`](../../hal/aidl-v1/VirtualCameraProvider.cpp) the id
list is now gated on a presence flag:

```cpp
if (mProducerPresent.load(std::memory_order_acquire)) {
    cameraIds->push_back(kVirtualCameraId);
} else {
    ALOGI("No producer registered - returning empty camera list");
}
```

**Presence transitions emit the status change:**

```cpp
void VirtualCameraProvider::setProducerPresent(bool present) {
    bool prev = mProducerPresent.exchange(present);
    if (prev == present) return;                 // only on a real 0↔1 edge
    if (mCallback) {
        mCallback->cameraDeviceStatusChange(
            kVirtualCameraId,
            present ? CameraDeviceStatus::PRESENT
                    : CameraDeviceStatus::NOT_PRESENT);
    }
}
```

**Who calls `setProducerPresent`?** The service (Lesson 4), because *it* knows
when producers come and go. It watches the count crossing 0↔N — on registration,
on unregistration, and (critically) on death:

```java
// service/VirtualCameraService.java  (shape)
// registerCamera:  if (mCameras.size() was 0) notifyHalAvailability(true);
// removeCamera:     if (mCameras.size() now 0) notifyHalAvailability(false);
callback.asBinder().linkToDeath(() -> removeCamera(id), 0);   // death → remove → false
```

That `linkToDeath` from Lesson 4 is doing real work here: a producer that
*crashes* removes the camera just as cleanly as one that unregisters politely.

## The consumer side: waiting instead of assuming

Because the camera may not exist yet, the viewer app is written to **wait for
it** rather than assume it. In
[`VCamViewer/.../MainActivity.java`](../../platform/apps/VCamViewer/src/com/example/vcamviewer/MainActivity.java):

```java
mCameraManager.registerAvailabilityCallback(new AvailabilityCallback() {
    @Override public void onCameraAvailable(String id) {
        if (VIRTUAL_CAMERA_ID.equals(id)) openCamera();   // appeared → open it
    }
    @Override public void onCameraUnavailable(String id) { /* gone / in use */ }
}, handler);
// and on the open camera:  onDisconnected() → producer went away → back to waiting
```

Start order no longer matters: launch the viewer first and it sits on "waiting
for virtual camera"; start the producer and the camera appears, the callback
fires, the viewer opens it.

## Watch it happen

On a running device:

```
# no producer yet:
$ adb shell dumpsys media.camera | grep "maps to"
    Device 0 maps to "0"
    Device 1 maps to "1"                 ← no 100

# start the producer, then look again:
$ adb shell am start-foreground-service -n com.example.vcamproducer/.VCamProducerService
$ adb shell dumpsys media.camera | grep "maps to"
    Device 2 maps to "100"               ← appeared

# logcat shows the whole chain in a few milliseconds:
#   setProducerAvailable(true) → Virtual camera ... -> PRESENT
#   → VCamViewer: Virtual camera AVAILABLE → Camera opened: 100
```

Measured on Cuttlefish: **≈7 ms** from registration to the viewer auto-opening;
**≈35 ms** from a producer force-kill to the camera being removed and clients
getting `onDisconnected`.

## What broke for real

* **Availability has to be *pushed*, not polled.** cameraserver only reacts to
  `cameraDeviceStatusChange`; there's no "is it there yet?" the HAL can cheaply
  poll. So the transition has to travel service→HAL exactly on the edges, and
  after a HAL restart the platform side must **re-sync** the current state (else
  the framework and HAL disagree about whether 100 exists).
* **A stale shared library crash-looped the HAL.** When the service→HAL interface
  changed, the HAL's implementation `.so` was rebuilt but a dependent shared lib
  wasn't shipped alongside it — the HAL aborted at load with a missing
  `onTransact` symbol and camera 100 never came back. *Lesson: when a shared AIDL
  library changes, everything that links it must ship together.*

## Takeaways

- A provider makes a camera **appear/disappear** with
  `cameraDeviceStatusChange(PRESENT/NOT_PRESENT)` — the USB-webcam mechanism.
- Tie it to producer registration so **availability events mean "a producer is
  live"**, and start-order stops mattering.
- It's **push-based**: emit on 0↔1 edges (including `linkToDeath`), keep
  `getCameraIdList` in agreement, and re-sync after a restart.
- Consumers should **wait for availability**, not assume presence.

We now have a complete, dynamic virtual camera — but everything so far assumed
the pieces can call each other freely. To actually ship it across the Treble wall
(Lesson 5) and make it *updatable*, we need a stable contract between the halves.

[← Lesson 5](05-treble-wall.md) · [Lesson 7: A frozen AIDL boundary →](07-frozen-boundary.md)
