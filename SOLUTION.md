# Virtual Camera — Solution Overview

A **virtual camera HAL for AOSP** that lets an ordinary app act as a camera: the app
produces frames (rendered, streamed, or captured), and those frames are delivered to
any standard **Camera2** consumer on the device as if they came from real hardware. A
companion **virtual microphone** and **virtual display** follow the same pattern.

This document is the concise-but-detailed map of *how the solution works*. For deep
dives see the component READMEs (`v2-shared-memory/`, `virtual-mic/`, `virtual-display/`).

---

## 1. The core idea

```
   Renderer app  ──frames──▶  Platform/Framework service  ──▶  Camera HAL (provider)  ──▶  Camera2 apps
   (producer)                 (surface / buffer broker)        (registers camera id 100)     (consumer)
```

- The **HAL** registers an extra camera device (id **100**) with the camera framework,
  advertising standard formats (`IMPLEMENTATION_DEFINED`, `RGBA_8888`, `YCbCr_420_888`)
  at 640×480 … 3840×2160 @ up to 30 fps.
- A **broker service** connects producer apps to the HAL and hands out the surface or
  buffer the app renders into.
- Any Camera2 client (including the OS camera app) then opens camera 100 and receives
  the produced frames — no consumer-side changes required.

---

## 2. Layered architecture

| Layer | Responsibility | Where |
|-------|----------------|-------|
| **HAL core** | Camera device/session, metadata, frame sources | `.../provider/virtual/core/` |
| **AIDL adapter** | Bind the core to a specific Camera AIDL version | `.../virtual/aidl/` (A15) · `aidl` + `platform-aidl` (A13) |
| **Broker service** | Register producers, broker surfaces/buffers to the HAL | A15: `frameworks/base … VirtualMediaService` · A13: `.../virtual/platform-service/` |
| **Renderer client** | App-side glue to the broker | A15: `VirtualMediaClient` (reflection) · A13: `IVirtualCameraService`/`Callback` |
| **Test apps** | Produce + view frames for validation | A15: `unified-test/` · A13: `.../virtual/apps/{VCamProducer,VCamViewer}` |

The HAL core is **AIDL-version-independent**; the thin adapter layer is what differs
between Android releases (see §5).

---

## 3. Frame-delivery paths (how a frame gets from app to HAL)

The project implements several transports, in increasing order of efficiency. All end
at the HAL, which converts/copies as needed and satisfies the Camera2 capture request.

1. **Shared memory (v1)** — app renders into a buffer, the HAL `memcpy`s it out of an
   `ashmem` region. Simple; `memcpy` bandwidth caps ~4K@30. `core/VirtualCameraFrameSource.cpp`.
2. **Buffer hand-off** — app renders into a local `ImageReader`, ships each
   `HardwareBuffer` to the broker (`sendHardwareBuffer`, A15). No file copy, but a Binder
   hop per frame.
3. **Zero-copy ring (v2)** — a pre-allocated pool of `AHardwareBuffer`s shared once over
   a Unix socket, cycled through a lock-free control ring with GPU fences; **no per-frame
   Binder, no memcpy**. `v2-shared-memory/SharedBufferPool.h`, `core/VirtualCameraFrameSourceV2.cpp`.
4. **Service-provided Surface (preferred)** — the broker owns the `Surface`/BufferQueue
   and hands it to the app; the app renders straight into it and the broker forwards to
   the HAL. This is the cleanest model and the **A13 native design**
   (`onStreamsConfigured(StreamConfig[], Surface[])` → *app → Surface → HAL BufferQueue*).

> Note: paths 3–4 are the same underlying primitive as an Android `Surface`
> (a gralloc/`AHardwareBuffer` BufferQueue). The custom v2 ring exists mainly to avoid a
> `libgui` dependency across the vendor boundary and to drop per-frame Binder.

---

## 4. The service-provided-Surface flow (the key validation target)

**A13 (native):**
```
producer app registers (IVirtualCameraService)
  → HAL configures streams, returns Surfaces via IVirtualCameraCallback.onStreamsConfigured(Surface[])
  → app renders into those Surfaces
  → HAL BufferQueue → Camera2 consumer
```
Exercised end-to-end by `apps/VCamProducer` (renders) + `apps/VCamViewer` (Camera2 preview).

**A15 (VirtualMediaService):**
```
app: vmClient.createVideoSurface(w,h)  → Surface owned by VirtualMediaService (its own ImageReader)
  → app renders into it (lockHardwareCanvas)
  → service onSurfaceFrameAvailable → forwards to Camera HAL → Camera2 consumer
```
`unified-test/` renders a bouncing-ball pattern into the service Surface and shows the
Camera2 result side-by-side; success = the ball appears in the right-hand preview.

---

## 5. Deployment targets — A13 vs A15

The two trees share the HAL **core** but differ at the adapter/service layer:

| | **Android 13 (TP1A / SDK 33)** — `/mnt/micron/aosp-a13` | **Android 15 (BP1A / SDK 35)** — `/mnt/micron/aosp` |
|---|---|---|
| Camera AIDL adapter | `aidl` + `platform-aidl` (V1-era) | `aidl` (V2-era) |
| Broker service | in-tree `platform-service/` (dedicated) | `frameworks/base` **`VirtualMediaService`** (system service `virtual_media`) |
| Surface model | `IVirtualCameraCallback.onStreamsConfigured(Surface[])` (native) | `createVideoSurface` + shared-mem + `sendHardwareBuffer` |
| Test apps | in-tree `apps/VCamProducer`, `apps/VCamViewer` | `unified-test/` (Gradle APK) |
| Compat shims | `HandleImporter`/mapper-type shims, `VCAM_COMPAT_A13` | none |

A13 is the cleaner, Surface/BufferQueue-first design and is the current **first
deployment target**. The two trees coexist without conflict (separate source + `out/`);
only one Cuttlefish VM can run at a time on the default instance (use
`--base_instance_num=2` to run both).

---

## 6. Build & run

**HAL (either tree):**
```bash
source build/envsetup.sh
lunch aosp_cf_x86_64_phone-userdebug           # A13 (2-part combo)
# lunch aosp_cf_x86_64_phone-ap3a-userdebug    # A15 (3-part combo)
m android.hardware.camera.provider-virtual-service   # + virtual-camera-core
```
**Test APK (A15 unified-test):** `./gradlew assembleDebug` → `adb install`.
**On device:** boot Cuttlefish, install/enable the HAL + broker, then run the
producer/viewer (or unified-test) and confirm camera 100 enumerates and shows the
rendered frames.

---

## 7. Status (2026-08-28) — A13 round trip VALIDATED end-to-end ✅

- 🏆 **Full producer round trip working on A13/Cuttlefish**: `VCamProducer` registers with
  `VirtualCameraService`, receives the HAL-relayed Surface (`onStreamsConfigured`), renders
  ("AIDL frame N" + moving ball), and `VCamViewer` displays those frames via **camera 100**
  over standard Camera2. With no producer, the HAL serves scrolling SMPTE color bars.
- ✅ Cuttlefish boots fine on the current host kernel. The earlier "won't boot" episode was
  **stale orphaned crosvm processes holding instance locks/ports** (cleanup must match the
  arch-subdir binary `bin/x86_64-linux-gnu/crosvm` and free ports 6520/6600/8443) — not a
  kernel/crosvm incompatibility. Use `--gpu_mode=gfxstream` (SwiftShader crashes app HWUI).
- ✅ HAL builds clean on both A15 and A13; A15 `unified-test` renders into the
  service-provided Surface (`createVideoSurface` + `lockHardwareCanvas`); APK built.
- Key fixes that unlocked the A13 round trip (details in the a13-platform-aidl branch README):
  `SW_WRITE_OFTEN` on the HAL BufferQueue (producer `lockCanvas`), gralloc mapper init is
  once-per-process (restart HAL after policy change: `ctl.restart camera-provider-virtual`),
  VINTF fragment `type=` must match its partition, and the `virtual_renderer/0` service-name
  label gates which domain may register the provider.
- Open items: proper sepolicy for `/dev/dri` access (demo runs permissive), producer transport
  for the vendor-packaged variant (v1/v2 unix sockets), 30fps pacing polish.

---

## 8. Where the code lives

- **Canonical repo:** GitHub `jsuppe/aosp-virtual-camera` (branch `main`).
- **A15 integration tree:** `/mnt/micron/aosp/hardware/interfaces/camera/provider/virtual/`
  (+ `frameworks/base/.../virtualmedia/`).
- **A13 integration tree:** `/mnt/micron/aosp-a13/hardware/interfaces/camera/provider/virtual/`
  (`core/`, `aidl/`, `platform-aidl/`, `platform-service/`, `apps/`).
- This working copy (`~/.openclaw/workspace-evelyn/virtual-camera/`) mirrors the repo and
  holds the test-app sources and per-component docs.
