# Lesson 7 — A frozen AIDL boundary

**What you'll understand:** how platform and vendor exchange handles and control
messages through a versioned contract, and what compatibility does not promise.

## The boundary

The BufferQueue stays platform-side, where `libgui` is available. The vendor HAL
receives buffer handles, descriptions and fences, not pixel bytes or a private
platform `Surface` representation.

The contract is [IVirtualCameraHal.aidl](../../stable-aidl/android/hardware/virtualcamera/hal/IVirtualCameraHal.aidl),
with the reverse channel in
[IVirtualCameraHalCallback.aidl](../../stable-aidl/android/hardware/virtualcamera/hal/IVirtualCameraHalCallback.aidl).
Both use `@VintfStability`; [Android.bp](../../stable-aidl/Android.bp) declares
`stability: "vintf"` and frozen versions **1 through 6**.

The system calls the HAL; the HAL uses the supplied callback for lifecycle
notifications. Compatible stable AIDL wire types, VINTF declarations and SELinux
access are all required. Sending a `NativeHandle` does not by itself make an
arbitrary buffer format or graphics-driver combination portable.

## How the interface evolved

| Version | Addition |
|---------|----------|
| V1 | Global producer availability, frame handle delivery and lifecycle callback |
| V2 | Acquire-fenced delivery and retirement-fence return for the previous source |
| V3 | Per-camera slots and `getMaxCameras()`; legacy unslotted calls address slot 0 |
| V4 | Configuration-token-bound frame submission and lifecycle callbacks |
| V5 | Immutable producer descriptors, registration-matched removal and configuration readiness |
| V6 | Lifecycle callbacks also carry producer registration identity |

The snapshots in [aidl_api](../../stable-aidl/aidl_api/android.hardware.virtualcamera.hal/)
are the exact contracts. Comments describing only V1–V3 elsewhere in the tree
are historical, not the full present protocol.

**Current compatibility floor:** `connectHal()` in
[VirtualCameraRelayJni.cpp](../../platform-jni/VirtualCameraRelayJni.cpp) rejects
HAL versions below **6**. Older methods and dispatch branches remain, but this
platform client does not automatically fall back to V1/V2/V3 HALs. Stable AIDL
preserves older wire contracts; it does not require a newer client to accept an
older server lacking features that client needs.

Do not confuse this custom interface's V6 with the standard camera provider's
interface version, the `hal/aidl-v1` source-directory name, or APEX package
version 8 in the reviewed source. These are different version spaces.

## The current exchange

Conceptual sequence; consult the linked AIDL for exact signatures:

```text
platform → HAL: registerProducer(slot, descriptor, owner callback)
HAL → platform: onStreamsConfiguredForProducer(slot, registration,
                                              configurationToken, width, height, fps)
platform → app: negotiated StreamConfig + producer Surface
app → platform: setProducerReady(callback, connectionToken, configurationToken, ready)
platform → HAL: setConfigurationReady(slot, configurationToken, ready)
platform → HAL: queueFrameForConfiguration(slot, configurationToken,
                   handle, dimensions, stride, format, usage, timestamp, acquireFence)
HAL → platform: retirement fence for the previously queued source
```

The platform connection epoch scopes configuration tokens to one HAL connection.
The registration identity prevents old lifecycle events from reaching a later
owner of the same slot. The frame fence prevents reading before drawing has
finished. These solve different problems; none substitutes for another.

The vendor imports the handle with `AHardwareBuffer_createFromHandle` and holds
source leases while capture uses it. The JNI relay retains the corresponding
BufferQueue item until retirement is safe. Cloning fds alone does not prevent the
producer from overwriting a recycled buffer.

## Freezing a new version

After a deliberate compatible interface change in an integrated AOSP tree:

```sh
m android.hardware.virtualcamera.hal-update-api
m android.hardware.virtualcamera.hal-freeze-api
```

Review generated snapshots/hashes and version metadata; preserve all existing
frozen snapshots. Follow the target release's stable-AIDL build rules and copy
the reviewed API artifacts back to this source repository. These are developer
API-maintenance steps, not commands needed by an app or for every APEX update.
A new required interface version can require a coordinated platform/HAL update.

## What broke for real

- An AIDL package segment named `virtual` generated an invalid C++ namespace:
  it is a C++ keyword. The package now uses `virtualcamera`.
- Similar namespace names shadowed the core namespace; explicit leading `::`
  removed the ambiguity.
- Slot-only lifecycle callbacks could target a replacement producer. V6 adds
  registration identity; retaining old wire methods alone cannot solve that race.

## Takeaways

- Freeze the cross-partition contract and negotiate/check versions explicitly.
- The current platform relay requires **V6**, not just any historical version.
- Handles avoid pixel serialization; identity and fences preserve correctness.
- Stable AIDL enables compatible component updates, not arbitrary base-image,
  ABI, graphics-stack or policy compatibility.

[← Lesson 6](06-dynamic-availability.md) · [Lesson 8: Shipping as a vendor APEX →](08-vendor-apex.md)
