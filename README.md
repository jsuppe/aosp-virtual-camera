# AOSP Virtual Camera — An Explainer

This project adds a **virtual camera** to Android: a camera device that any
normal camera app can open (id `100`), whose "sensor" is not hardware but
**another app**. A producer app registers, receives a drawing `Surface`, and
whatever it renders shows up — live — in any Camera2 client on the device.

```
VCamProducer (a normal app) ──registerCamera()──▶ VirtualCameraService (system_server)
        ▲                                                 │
        └──── onStreamsConfigured(Surface[]) ◀────────────┘   relayed from the HAL
 app draws into the Surface ──▶ HAL-owned BufferQueue ──RGBA→YUV──▶ camera id 100
                                              └──▶ Camera2 API ──▶ any camera app  ✅
```

This README is written as an **explainer**: it assumes you know Android app
development roughly, but *not* the camera stack, gralloc, Binder/HAL plumbing,
or how a device tree integration works. Concepts are introduced as they are
needed. If you already know the platform, skim the headers and jump to
[§6 Integration](#6-the-integration-process) and [§8 Field notes](#8-field-notes--the-bugs-you-would-otherwise-hit).

**Status:** the full round trip above is validated end-to-end on Android 13
(Cuttlefish emulator, real-GPU mode). With no producer registered, the camera
shows animated SMPTE color bars, so it is always visibly alive.

---

## 1. How Android cameras actually work

When an app calls the **Camera2 API** (`CameraManager.openCamera("100")`),
nothing talks to hardware directly. The call travels through several
*processes*:

```
 Camera app ──Binder──▶ cameraserver ──Binder/AIDL──▶ camera HAL process ──▶ hardware
 (Camera2 API)          (framework's                  (vendor-supplied
                         camera broker)                "driver" service)
```

* **cameraserver** is the framework's camera broker. It enumerates every
  *camera provider* registered with the system and multiplexes apps onto them.
* A **HAL** (Hardware Abstraction Layer) is just a long-running native service
  that implements a standard interface — here
  `android.hardware.camera.provider.ICameraProvider`. Real phones have one per
  camera chip vendor. **Our trick: we implement this same interface in
  software.** cameraserver cannot tell the difference; that is why unmodified
  camera apps work.
* The camera does not "push" video. For every frame, the framework sends the
  HAL a **capture request** containing an *empty output buffer*; the HAL fills
  the buffer and hands it back. The app's preview is just a stream of filled
  buffers.

So a virtual camera needs to do exactly three things:

1. register as a camera provider (so camera `100` enumerates),
2. answer capture requests by filling the supplied buffers,
3. get pixels *from somewhere* to fill them with — for us, another app.

Step 3 is where all the interesting Android plumbing lives.

## 2. Buffers, gralloc, and what "zero-copy" means

### Graphics buffers are shared memory with a personality

A 4K RGBA frame is ~33 MB. At 30 fps that is a gigabyte per second — you cannot
afford to copy frames around, and you especially cannot afford to send them
*through* IPC. Android's answer is the **graphics buffer**
(`GraphicBuffer` / `AHardwareBuffer` / `buffer_handle_t` — three names for the
same thing at different API levels): a chunk of memory allocated by
**gralloc**, the graphics allocator HAL.

Key properties:

* Allocated once, then **shared across processes by handle** (file
  descriptors), not by copying contents. Passing a frame between processes
  costs nanoseconds, not megabytes. This is what people mean by **zero-copy**.
* Allocated with **usage flags** that declare who will touch it and how:
  `GPU_COLOR_OUTPUT` (GPU renders into it), `SW_READ_OFTEN` /
  `SW_WRITE_OFTEN` (CPU maps it), `CAMERA_OUTPUT`, etc. The allocator picks a
  memory type/layout that satisfies all of them. **Get the flags wrong and a
  consumer or producer simply cannot map the buffer** — you will meet this in
  §8.
* A process cannot use a received handle directly: it must **import** it
  through the **gralloc mapper** (on Cuttlefish: *minigbm*), which validates
  the handle and maps it into the local address space. Import failing is the
  single most common way a camera pipeline silently produces black frames.

### Surface & BufferQueue: Android's producer/consumer conveyor

Nobody juggles raw buffers by hand. Android wraps them in a **BufferQueue**: a
small ring (3–4 buffers) with a producer end and a consumer end.

```
 producer (app drawing)                    consumer (whoever reads frames)
   dequeueBuffer() ──▶ [ B0 ][ B1 ][ B2 ] ──▶ acquireBuffer()
   queueBuffer()   ◀──   recycled        ◀── releaseBuffer()
```

A **`Surface` is simply the producer end of a BufferQueue**, packaged as an
object you can hand to another process over Binder. When you give an app a
Surface, you are really giving it "permission to dequeue, draw into, and queue
buffers that *I* own and consume." The buffers themselves never move — only
handles and indices do. Every preview, every video encoder, every screen
composition in Android rides on this one primitive.

Our design leans on it directly: **the HAL owns a BufferQueue per stream and
hands its Surface to the producer app.** The app draws; the HAL consumes. One
copy total (the RGBA→YUV conversion into the camera output buffer), no IPC per
frame.

## 3. The IPC and partition landscape (Binder, Treble, VINTF, SELinux)

Four platform mechanisms shape *where* code is allowed to live and *who* may
talk to whom. Each one bit us during bring-up, so they are worth understanding.

* **Binder / AIDL** — Android's IPC. Interfaces are declared in `.aidl` files;
  the build generates client/server stubs. Services register by *name* with
  `servicemanager`; clients look them up. Our three interfaces:
  `IVirtualCameraService` (apps → service), `IVirtualCameraCallback`
  (service → app, carries the Surface), `IVirtualCameraManager` (HAL ↔
  service).
* **Treble partitions** — `/system` and `/system_ext` hold platform code;
  `/vendor` holds hardware code. They are built and updated independently, so
  the platform↔vendor boundary is a *wall*: vendor code may not link platform
  internals (e.g. `libgui`, the BufferQueue library) and vice versa. This
  single constraint drives our biggest design decision (§5).
* **VINTF manifests** — XML files declaring which HAL interfaces exist on each
  side of the wall. `servicemanager` *enforces* them: a HAL whose name is not
  declared cannot register. Fragments are typed per side —
  `type="framework"` for system/system_ext, `type="device"` for vendor — and a
  mistyped fragment can corrupt the whole manifest (§8, boot-wedge story).
* **SELinux** — every process runs in a *domain*, every file/device/service
  name has a *label*, and policy enumerates the allowed (domain → label)
  actions. Three places it gates us: opening the GPU device (`/dev/dri`, which
  gralloc's mapper needs), registering our service name
  (`virtual_renderer/0`, labeled `virtual_camera_provider_service`), and app
  access to camera. Denials log as `avc: denied` lines in dmesg/logcat.

## 4. The components of this project

| Piece | Runs as | Role |
|---|---|---|
| **HAL** (`hal/core` + `hal/aidl-v1`) | native service, `/system_ext/bin/hw` | Implements `ICameraProvider` for camera id 100; answers capture requests; owns the per-stream BufferQueue; converts RGBA→YUV into the output buffers |
| **`AidlFrameSource`** (`hal/core`) | inside the HAL | The bridge: creates the BufferQueue, sends its Surface up to the service, consumes the producer's frames |
| **VirtualCameraService** (`platform/services`) | inside `system_server` | Registry: producers call `registerCamera()`; when the HAL reports a stream, relays the Surface to the producer via `IVirtualCameraCallback.onStreamsConfigured()` |
| **AIDL definitions** (`platform/aidl-lib`) | build-time | The three interfaces above, built both as a Java lib (service, apps) and a C++ lib (HAL) |
| **VCamProducer** (`platform/apps`) | normal app (foreground service) | Demo producer: registers, receives the Surface, draws an animated pattern with `lockCanvas()` |
| **VCamViewer** (`platform/apps`) | normal app | Demo consumer: plain Camera2 client that opens camera 100 and shows the preview |
| **sepolicy** (`platform/sepolicy`) | build-time | Domain for the HAL, label for the service name, app access rules |

### The life of one frame

1. VCamViewer opens camera 100 → cameraserver connects to our HAL and calls
   `configureStreams` (e.g. 3840×2160 RGBA).
2. The HAL's `AidlFrameSource` asks `VirtualCameraService` "any producer
   registered?" — if yes, it builds a BufferQueue for the stream and calls
   `notifyStreamsConfigured(cameraId, streams, surfaces)`.
3. The service relays that Surface to VCamProducer's callback. The producer
   spins up a render thread: `surface.lockCanvas()` → draw → `unlockCanvasAndPost()`,
   ~30 times a second. Each post queues a buffer into the HAL's queue —
   cross-process, zero-copy.
4. Independently, cameraserver streams capture requests at the HAL. Per
   request the HAL: **imports** the framework's output buffer via the gralloc
   mapper (cached after first use), **acquires** the newest producer buffer
   from its BufferQueue, converts RGBA→YUV into the output buffer, returns it.
5. cameraserver hands the filled buffer to VCamViewer's preview Surface.
   Pixels drawn by one app appear in another, through the real camera stack.
6. No producer registered? Step 4 falls back to drawing scrolling color bars,
   so the camera is never black.

## 5. The one hard design decision: which side of the Treble wall?

The HAL wants two things that live on **opposite sides** of the platform/vendor
wall:

* **BufferQueue/Surface relay** → needs `libgui` + framework AIDL parcelables
  → only linkable by **platform** code (`/system_ext`).
* **Frictionless gralloc** → the mapper and its `/dev/dri` access are
  **vendor**-side; a vendor HAL (domain `hal_camera_default`) gets them for
  free, a system_ext process needs explicit SELinux allowances.

Both packagings exist in this repo:

* **Platform build** (default, this branch's validated path):
  `system_ext`, relay enabled. Gralloc works once policy admits the HAL to
  `/dev/dri` (demo shortcut: permissive + HAL restart, see §8.1).
* **Vendor build** (`platform/vendor-variant/`): `vendor: true`, runs in
  `hal_camera_default`, gralloc works out of the box — but the relay must be
  compiled out (`libgui` is not vendor-linkable), so producers would feed
  frames via the HAL's unix-socket sources (`VirtualCameraFrameSource[V2]`)
  instead. Likely the production shape; the socket path needs a
  vendor-writable location before it works there.

## 6. The integration process

AOSP has no plugin mechanism — integrating a HAL means **copying source into
the device tree and rebuilding the image**. `scripts/integrate-a13-platform.sh
<aosp_root>` automates it; here is what it does and why each piece exists:

1. **HAL sources** → `hardware/interfaces/camera/provider/virtual/{core,aidl}`
   plus `Android.bp` files (from `platform/bp/`). `Android.bp` is Soong's
   build file: module names, sources, and — critically — the dependency lists
   and the partition switch (`system_ext_specific: true` vs `vendor: true`).
2. **AIDL + service + apps** → `platform-aidl/`, `platform-service/` (compiled
   into `system_server`), and the two demo apps (preinstalled to
   `system_ext/app`).
3. **init `.rc`** → tells `init` to start our binary at boot, as which user,
   in which service class. Installed to `<partition>/etc/init/`.
4. **VINTF fragment** → declares `ICameraProvider/virtual_renderer` so
   servicemanager will accept our registration. Installed to
   `<partition>/etc/vintf/manifest/`. *Its `type=` must match the partition.*
5. **sepolicy** → into the Cuttlefish device policy dir: the
   `virtual_camera_hal` domain, the `virtual_camera_provider_service` service
   label, and the allow rules binding them.
6. Then a full image build (`m`) — first time only; afterwards you can rebuild
   just the modules and `adb push` them (see §7).

The A15 flow (`integrate.sh`, `main` branch) is the same idea with a vendor
HAL and a `VirtualMediaService` instead of the relay.

## 7. Build, run, demo

```bash
# one-time: integrate + full build (Cuttlefish target)
./scripts/integrate-a13-platform.sh /path/to/aosp-a13
cd /path/to/aosp-a13 && source build/envsetup.sh && lunch aosp_cf_x86_64_phone-userdebug && m

# boot (real GPU — SwiftShader crashes app rendering, see §8.4)
launch_cvd --daemon --gpu_mode=gfxstream --cpus=4 --memory_mb=4096

# demo-mode SELinux (see §8.1 for why the restart is mandatory)
adb root
adb shell setenforce 0
adb shell setprop ctl.restart camera-provider-virtual

# run: producer FIRST, then any camera app
adb shell pm grant com.example.vcamviewer android.permission.CAMERA
adb shell am start-foreground-service -n com.example.vcamproducer/.VCamProducerService
adb shell monkey -p com.example.vcamviewer -c android.intent.category.LAUNCHER 1
# → viewer shows the producer's animated frames ("AIDL frame N" + moving ball)
```

Iterating on the HAL without reflashing:
`m <module>` → `adb root; adb remount` (needs one reboot the first time) →
`adb push` the `.so`/binary → reboot. Verify pushes loudly — a read-only
partition fails silently if you pipe to /dev/null.

## 8. Field notes — the bugs you would otherwise hit

Each of these cost real debugging time; they are the practical distillation of
§2–§3.

1. **Gralloc mapper initialization happens once per process.** minigbm opens
   the DRM device in its constructor at first use. If SELinux denies
   `/dev/dri` *at that moment*, the mapper is dead for the life of the process
   — every later import logs `Failed to import buffer. Driver is
   uninitialized`, and **`setenforce 0` afterwards does not heal it.** Relax
   policy, then **restart the HAL** (`ctl.restart camera-provider-virtual`).
   Productization TODO: grant `virtual_camera_hal` access to `gpu_device` in
   sepolicy instead of running permissive.
2. **Usage flags must cover every accessor.** The HAL first allocated its
   BufferQueue with only `SW_READ_OFTEN` (it reads frames on the CPU). The
   producer draws with software `lockCanvas()` — a CPU *write* — which threw
   `IllegalArgumentException` until `SW_WRITE_OFTEN` was added to the
   consumer's usage. Rule: the buffer's flags are the union of *everyone's*
   access pattern, producer and consumer alike.
3. **A mistyped VINTF fragment can wedge boot.** Moving the service to
   /vendor while leaving the fragment `type="framework"` corrupted the device
   manifest → servicemanager rejected *every* vendor HAL → keymint never
   registered → keystore2 hung → boot never completed. The failure appears
   totally unrelated to the change; check fragment `type=` first when a
   partition move breaks boot.
4. **Emulator GPU mode matters.** With `--gpu_mode=guest_swiftshader`
   (software GL), app HWUI rendering segfaults under load — the *viewer*
   crashes and takes the demo with it. `gfxstream` (host-GPU passthrough) is
   solid.
5. **Service-name labels gate registration.** `virtual_renderer/0` is labeled
   `virtual_camera_provider_service`, and policy only lets the
   `virtual_camera_hal` domain register it. Run the HAL in any other domain
   (e.g. as a vendor service in `hal_camera_default`) and it exits in a
   crash-loop with `avc: denied { add }` — relabel or extend policy to move it.
6. **Stale emulator state mimics real bugs.** Orphaned `crosvm` processes
   (note: the binary lives at `bin/x86_64-linux-gnu/crosvm`, which naive
   `pkill crosvm` patterns miss) hold instance disk locks and ports
   (6520/6600/8443); the next launch then fails in ways that look like kernel
   or driver incompatibilities. Before diagnosing anything exotic: kill all
   cuttlefish processes by exe path, `cvd reset -y`, remove
   `~/cuttlefish/instances` and `/tmp/cf_*`, confirm the ports are free.

## 9. Repository map

```
hal/core/        AIDL-independent engine: frame sources (BufferQueue relay,
                 v1 shm socket, v2 AHardwareBuffer socket), RGBA→YUV filler
                 (+ color-bar fallback), metadata builder
hal/aidl-v1/     Camera AIDL adapter used on A13 (provider/device/session)
hal/aidl-v2/     Same adapter for the newer Camera AIDL (A15 path)
platform/        A13 platform half: aidl-lib, services (system_server),
                 apps (VCamProducer/VCamViewer), bp/rc/vintf/sepolicy,
                 vendor-variant (alternative /vendor packaging)
scripts/         integrate-a13-platform.sh, build/test/launch helpers
v2-shared-memory/  zero-copy AHardwareBuffer pool + lock-free ring (design for
                 the socket-fed vendor path)
virtual-mic/, virtual-display/   companion virtual devices (same pattern)
unified-test/, sample-renderer/, camera-test/, test-app/   A15-era test apps
```

## 10. Where this goes next

* Proper sepolicy for `/dev/dri` (drop the permissive shortcut).
* Vendor-variant producer transport: move the socket path somewhere
  vendor-writable and wire a producer to `VirtualCameraFrameSourceV2`
  (zero-copy `AHardwareBuffer` ring — see `v2-shared-memory/`).
* 30 fps pacing in the HAL request loop.
* 4K60 via the v2 ring; multi-producer arbitration; virtual mic/display parity.

## License

Apache 2.0 (matching AOSP).
