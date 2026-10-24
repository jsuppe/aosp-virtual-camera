# Lesson 6 — Dynamic availability

**What you'll understand:** how producer registration publishes a camera, how
teardown removes it, and why presence is different from readiness or freshness.

## Three states, not one

The current stable-AIDL design separates:

1. **Registered/present:** the HAL accepted a producer descriptor for a slot,
   so a Camera2 device with those capabilities can be discovered.
2. **Configuration ready:** the producer accepted the negotiated source Surface
   and acknowledged its connection/configuration tokens.
3. **Fresh frame ready:** a new submission for that configuration exists and its
   producer fence has completed before the capture deadline.

A producer can be registered while not drawing. It can also stall after several
successful frames. Neither situation automatically removes its camera. Capture
requests fail rather than substituting cached frames or synthetic color bars.
Conversely, unregistering or producer death triggers device removal.

## Publication is per producer slot

[VirtualCameraProvider.cpp](../../hal/aidl-v1/VirtualCameraProvider.cpp) enumerates
`mSlotPresent[]` and sends status transitions through its provider callback:

```cpp
// Abridged from setSlotPresent; validation and locking omitted here.
const std::string id = ::virtualcamera::deviceIdForSlot(slot);
cb->cameraDeviceStatusChange(
        id, present ? CameraDeviceStatus::PRESENT
                    : CameraDeviceStatus::NOT_PRESENT);
```

`getCameraIdList` and the pushed transitions describe the same per-slot state.
Slots 0–3 map to Camera2 IDs 100–103. This is not the original global “at least
one producer exists” switch: removing producer 101 must not remove producer 100.
The legacy unslotted methods remain in the frozen interface, but they are not
the current broker's descriptor-based registration path.

The HAL validates a `ProducerDescriptor` before publishing it. The descriptor
contains a registration identity, source modes, output formats, facing,
orientation and stream-count limit. Metadata is derived from these declarations;
an advertised mode is not a measurement that the hardware sustains its rate.
See [VirtualCameraStableHal.cpp](../../hal/aidl-v1/VirtualCameraStableHal.cpp) and
[ProducerCapabilities.h](../../hal/core/ProducerCapabilities.h).

## Lifecycle and identity

The [broker](../../service/VirtualCameraService.java) assigns a slot and queues
HAL registration. Its integer registration ID is distinct from the Camera2 ID.
`linkToDeath` on the producer callback initiates removal if its process dies.

Removal in the current service follows this order:

1. Mark the slot closing and remove the registration from the broker's map.
2. Call generation/owner-matched `removeProducer` on the HAL.
3. Release the source Surface/queue and close producer state.
4. Only then finish closing the slot so another registration can reuse it.

This ordering prevents an old cleanup operation from dismantling a new producer
that happens to occupy the same slot. HAL callbacks carry registration and
configuration identities, and the platform also checks its HAL connection
epoch. Tokens from a dead HAL connection cannot identify work in its replacement.
Recovery reconnects and attempts to re-register current producers; it is not
an unlimited guarantee of transparent recovery in every failure scenario.

## A consumer waits for availability

A Camera2 consumer should observe `CameraManager.AvailabilityCallback`, discover
suitable IDs/capabilities, and handle open failure or disconnection. The root
[viewer](../../platform/apps/VCamViewer/src/com/example/vcamviewer/MainActivity.java)
is a reference; the Pi split viewer is target-specific.

Conceptual consumer logic:

```text
matching camera becomes available → attempt open/configure
open/configure fails               → handle failure, do not assume success
camera disconnects                 → close session/device, return to discovery
```

`onCameraUnavailable` does **not** uniquely mean producer death: a present camera
can be unavailable because it is in use or blocked by access policy. Availability
is a camera-access signal, not an assertion that the producer is drawing.

## Observe it on an integrated target

```sh
adb -s SERIAL shell dumpsys media.camera
adb -s SERIAL logcat -s VirtualCameraService VirtualCameraProvider
```

On A13, the producer service can be started explicitly:

```sh
adb -s SERIAL shell am start-foreground-service \
  -n com.example.vcamproducer/.VCamProducerService
```

On Pi/A17, use the installed producer launcher and its explicit start controls.
Observe publication, stream negotiation and readiness separately. Timing varies
with target and workload; old single-run registration/removal timings are not
current latency guarantees.

## What broke for real

- A delayed callback can arrive after a slot is reused. Checking just the slot
  number or a global frame counter is insufficient; retain originating identity.
- Updating an interface library without all its dependent binaries can produce
  missing-symbol failures before camera discovery ever begins.
- A frozen preview during a stall is not a successful capture: inspect errors
  and independently decoded source identities, not just window contents.

## Takeaways

- **Presence tracks accepted registration, not continuous frame production.**
- Readiness and fresh-frame admission are separate checks.
- Push device-status transitions, handle death, and guard teardown/recovery
  with registration, configuration and connection identity.
- Provider enumeration and consumer availability are related but not identical.

[← Lesson 5](05-treble-wall.md) · [Lesson 7: A frozen AIDL boundary →](07-frozen-boundary.md)
