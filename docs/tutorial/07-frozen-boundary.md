# Lesson 7 — A frozen AIDL boundary

**What you'll understand:** how to let the platform and vendor halves talk across
the Treble wall through a *stable, versioned* interface — and why "freezing" that
interface is what later makes the HAL independently updatable.

---

## The problem restated

Lesson 5 left us with a hard fact: the BufferQueue must live **platform-side**
(`libgui`), but the HAL that fills camera buffers wants to be **vendor-side** (for
gralloc). So frames have to cross the wall. Lesson 4 said "never send frames
through Binder" — and that still holds. The resolution:

> The **queue and consumer stay platform-side.** What crosses the wall is only
> the **handle** of each consumed frame (cheap, stable — Lesson 2) plus small
> control messages. Both travel over a dedicated AIDL interface.

But an interface *across the Treble wall* has a requirement ordinary app AIDL
doesn't: the two sides are built and shipped **separately** (that's the whole
point of Treble, and it's what makes the APEX in Lesson 8 possible). If the
platform and the vendor HAL can be updated on different schedules, their shared
interface must be **stable** — frozen, versioned, and never silently changed.

## `@VintfStability` and freezing

An AIDL interface marked `@VintfStability` is a **contract across the wall.** Its
wire format is hashed and versioned; the build refuses to let you change a frozen
version. You can add a *new* version, but you can't mutate version 1 out from
under someone still running it.

Our boundary interface —
[`stable-aidl/.../IVirtualCameraHal.aidl`](../../stable-aidl/android/hardware/virtualcamera/hal/IVirtualCameraHal.aidl):

```aidl
@VintfStability
interface IVirtualCameraHal {
    void setCallback(in IVirtualCameraHalCallback callback);
    void setProducerAvailable(boolean available);              // ← Lesson 6, across the wall
    void queueFrame(in NativeHandle buffer, int width, int height,
                    int stride, int format, long usage, long timestampNs);
}
```

Read the three methods against everything so far:

- `setProducerAvailable` — the availability push from Lesson 6, now crossing the
  wall (system_server → vendor HAL).
- `queueFrame` — a **frame handle** (`NativeHandle`) plus its description. This is
  the data plane crossing the wall *by handle*, exactly as Lesson 2 promised.
  Pixels don't move; a handle does.
- `setCallback` — registers the reverse channel
  (`IVirtualCameraHalCallback.onStreamsConfigured / onCameraClosed`), so the HAL
  can drive the platform side when a Camera2 consumer configures streams.

The direction is the normal Treble direction — **system calls *down* into the
HAL; the HAL answers through the registered callback.**

## Freezing it

Freezing is a real build step, and it leaves an artifact you can see:

```
stable-aidl/aidl_api/android.hardware.virtualcamera.hal/1/
    android/hardware/virtualcamera/hal/IVirtualCameraHal.aidl   ← the frozen copy
    .hash                                                        ← its fingerprint
```

Once frozen at V1, the platform image and the vendor HAL can each be rebuilt and
reshipped independently, and both are guaranteed to speak V1. Change the
interface and you must cut a V2 — a deliberate, visible act, which is precisely
the discipline you want at a boundary that separates two independently-updated
halves.

## The two endpoints

Each side implements its end of the frozen interface:

- **Vendor endpoint** —
  [`hal/aidl-v1/VirtualCameraStableHal.cpp`](../../hal/aidl-v1/VirtualCameraStableHal.cpp).
  It registers `IVirtualCameraHal/default`, and on `queueFrame` it turns the
  incoming handle into an `AHardwareBuffer` and keeps the newest frame:

  ```cpp
  AHardwareBuffer_createFromHandle(&desc, handle,
      AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE, &ahb);   // handle → AHB
  // store as "latest"; the capture path (Lesson 9) consumes it
  ```

- **Platform endpoint** —
  [`platform-jni/VirtualCameraRelayJni.cpp`](../../platform-jni/VirtualCameraRelayJni.cpp)
  (owned by `system_server` via
  [`VirtualCameraNative.java`](../../service/VirtualCameraNative.java)). It owns
  the BufferQueue from Lesson 3, and its frame-available listener forwards each
  consumed handle down through the frozen interface:

  ```cpp
  auto aidlHandle = ::android::dupToAidl(gb->handle);       // GraphicBuffer → NativeHandle
  hal->queueFrame(aidlHandle, gb->getWidth(), gb->getHeight(),
                  gb->getStride(), gb->getPixelFormat(),
                  gb->getUsage(), item.mTimestamp);         // across the wall
  ```

That's the complete data path: producer draws → platform BufferQueue consumes →
`queueFrame` carries the *handle* across the wall → vendor HAL holds it → the
capture loop (Lesson 1) fills the camera buffer from it. No pixels crossed Binder.

## What broke for real

* **Don't name an AIDL package after a C++ keyword.** The interface was first
  `android.hardware.camera.virtual` — which generates `namespace …::virtual` in
  C++, and `virtual` is a keyword. It simply won't compile. (AOSP's own is
  `android.companion.virtualcamera` for the same reason.) Renamed to
  `android.hardware.virtualcamera.hal`.
* **Namespace shadowing.** The new `…hardware::virtualcamera` AIDL namespace then
  *shadowed* the project's own `virtualcamera::` core namespace, so unqualified
  references bound to the wrong one. Fix: qualify with a leading `::`.
* **The freeze has a chicken-and-egg on release branches.** On a `REL` platform
  an unfrozen `@VintfStability` interface won't build (it demands a frozen
  version) but you can't freeze without building — resolved by setting an
  `owner:` on the interface and running the `-freeze-api` / `-update-api` steps.

## Takeaways

- Cross-wall interfaces must be **stable**: `@VintfStability`, versioned, and
  **frozen** so the two independently-shipped halves always agree.
- Frames cross the wall **by handle** (`queueFrame(NativeHandle …)`), never as
  pixels; control (availability, stream lifecycle) rides the same interface.
- Freezing (`aidl_api/.../1/`) is the enabling move for the next lesson: it's
  what lets the vendor half ship — and update — on its own.

The interface is frozen and the two halves talk cleanly across it. Now we can do
the thing that stability was for: package the entire vendor HAL as **one signed,
updatable file.**

[← Lesson 6](06-dynamic-availability.md) · [Lesson 8: Shipping as a vendor APEX →](08-vendor-apex.md)
