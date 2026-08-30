# AOSP Virtual Camera — An Explainer

This project adds a **virtual camera** to Android: a camera device (id `100`)
that any normal camera app can open, whose "sensor" is not hardware but
**another app**. A producer app registers, receives a drawing `Surface`, and
whatever it renders shows up — live — in any Camera2 client on the device.

```
producer app ──registerCamera()──▶ VirtualCameraService (system_server)
      ▲                                   │ owns the BufferQueue
      └────── Surface (producer end) ◀────┘
app draws into the Surface ──▶ consumed frames cross the Treble boundary
      zero-copy over a FROZEN AIDL ──▶ vendor-APEX HAL ──▶ camera id 100
                                              └──▶ Camera2 ──▶ any camera app ✅
```

The camera is **dynamic**: it *exists* only while a producer is registered
(registration adds it to the system, unregistration or producer death removes
it — so Camera2 availability events mean "a virtual camera is producing"),
and the HAL ships as a **vendor APEX**: the whole engine updates by replacing
one signed file.

This README is written as an **explainer**: it assumes rough familiarity with
Android app development, but *not* the camera stack, gralloc, Binder/HAL
plumbing, APEX, or device-tree integration. Concepts are introduced as they
are needed. Already know the platform? Skim the headers and jump to
[§6 APEX](#6-shipping-as-a-vendor-apex) and
[§9 Field notes](#9-field-notes--the-bugs-you-would-otherwise-hit).

**Status:** validated end-to-end on Android 13 (Cuttlefish, host-GPU mode) at
4K30 with producer/boundary/viewer frame counters in lockstep, including a
v1→v2 APEX update cycle. The producer renders with **OpenGL ES** and the HAL
composites on the **GPU with zero color conversion** on the RGBA path (steady
state: no pixel touches the CPU — §5b). (SELinux runs permissive in the demo;
proper policy is the top productization item, §11.)

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
* Cameras can come and go at runtime: a provider reports
  `cameraDeviceStatusChange(PRESENT / NOT_PRESENT)` and the framework adds or
  removes the device, firing `AvailabilityCallback` events to apps — the
  mechanism USB webcams use, and the one we use for dynamic registration.

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
  costs microseconds, not megabytes. This is what people mean by **zero-copy**.
* Allocated with **usage flags** that declare who will touch it and how:
  `GPU_COLOR_OUTPUT` (GPU renders into it), `SW_READ_OFTEN` /
  `SW_WRITE_OFTEN` (CPU maps it), `CAMERA_OUTPUT`, etc. The allocator picks a
  memory type/layout satisfying all of them. **Get the flags wrong and a
  producer or consumer simply cannot map the buffer** — see §9.
* A process cannot use a received handle directly: it must **import** it
  through the **gralloc mapper** (on Cuttlefish: *minigbm*), which validates
  the handle and maps it locally. Import failing is the single most common way
  a camera pipeline silently produces black frames.

### Surface & BufferQueue: Android's producer/consumer conveyor

Nobody juggles raw buffers by hand. Android wraps them in a **BufferQueue**: a
small ring (3–4 buffers) with a producer end and a consumer end.

```
 producer (app drawing)                    consumer (whoever reads frames)
   dequeueBuffer() ──▶ [ B0 ][ B1 ][ B2 ] ──▶ acquireBuffer()
   queueBuffer()   ◀──   recycled        ◀── releaseBuffer()
```

A **`Surface` is simply the producer end of a BufferQueue**, packaged as an
object you can hand to another process over Binder. Giving an app a Surface
really means "permission to dequeue, draw into, and queue buffers that *I* own
and consume." The buffers never move — only handles and indices do.

In this project, **the BufferQueue is owned by `VirtualCameraService` inside
system_server** (the platform side). Its producer end becomes the Surface the
app draws into; its consumer side never touches pixels — it forwards each
consumed frame's *gralloc handle* across the Treble boundary to the vendor
HAL. Why it must be arranged this way is §5.

## 3. The rules of the terrain: Binder, Treble, VINTF, SELinux

Four platform mechanisms shape *where* code may live and *who* may talk to
whom. Each one bit us during bring-up.

* **Binder / AIDL** — Android's IPC. Interfaces are declared in `.aidl` files;
  the build generates client/server stubs. Services register by *name* with
  `servicemanager`; clients look them up. AIDL interfaces can be marked
  **`@VintfStability` and frozen**: the API is hashed and versioned, and both
  sides can then be updated independently against the frozen contract — the
  foundation of our updatable-APEX story.
* **Treble partitions** — `/system` and `/system_ext` hold platform code;
  `/vendor` holds hardware code, built and updated independently. The boundary
  is a *wall*: vendor code cannot link platform internals (e.g. `libgui`, the
  BufferQueue library) and vice versa.
* **VINTF manifests** — XML declaring which HAL interfaces exist on each side.
  `servicemanager` *enforces* them: an undeclared HAL cannot register.
  Fragments are typed per side (`type="framework"` vs `type="device"`), and a
  mistyped fragment can corrupt the whole manifest (§9, the boot-wedge story).
* **SELinux** — every process runs in a *domain*; every file, device, and
  service *name* has a *label*; policy enumerates allowed (domain → label)
  actions. It gates our HAL's access to the GPU device (`/dev/dri`, needed by
  the gralloc mapper) and which domain may register each service name.
  Denials log as `avc: denied` in dmesg/logcat.

## 4. The architecture (and how it got here)

The design went through three honest iterations — each is still in the tree,
because each teaches something:

| # | Where the HAL lives | Frame path | Verdict |
|---|---|---|---|
| 1 | `/system_ext` (platform) | HAL owns BufferQueue, relays Surface up via platform AIDL (`hal/core/AidlFrameSource`) | Works, but gralloc needs SELinux exceptions and nothing is updatable |
| 2 | `/vendor`, loose files | relay compiled out; unix-socket frame sources | gralloc "just works" (`hal_camera_default` domain) but no Surface relay |
| 3 | **vendor APEX + frozen AIDL** (shipping) | platform owns BufferQueue; frames cross as handles over `android.hardware.virtualcamera.hal` V1 | **Both halves work, and the HAL is updatable** |

### The shipping components

| Piece | Runs as | Role |
|---|---|---|
| **stable-aidl/** `android.hardware.virtualcamera.hal` | build-time (frozen V1) | The Treble contract: `setProducerAvailable`, `queueFrame(NativeHandle+desc)`, `IVirtualCameraHalCallback.onStreamsConfigured/onCameraClosed` |
| **HAL** (`hal/core` + `hal/aidl-v1` + `VirtualCameraStableHal`) | vendor APEX, domain `hal_camera_default` | Implements `ICameraProvider` (camera 100) *and* `IVirtualCameraHal`; keeps the newest pushed frame as an `AHardwareBuffer`; fills capture buffers on the GPU (`GpuCompositor`, zero conversion) with a CPU converter fallback |
| **VirtualCameraService** (`service/`) | inside `system_server` | Producer registry (`registerCamera`), availability push, stream relay orchestration |
| **platform-jni/** (`libvirtualcamera_relay_jni`, via `VirtualCameraNative`) | inside `system_server` | Owns the BufferQueue; wraps the producer end as the app-facing Surface; forwards consumed gralloc handles down over the frozen AIDL; reconnects on HAL death (APEX update!) |
| **VCamProducer / VCamViewer** (`platform/apps`) | normal apps | Demo producer (renders with `lockCanvas`) and availability-driven Camera2 viewer |
| **apex/** | `/vendor/apex` | The deliverable: one signed file containing the whole HAL (§6) |

### The life of one frame (shipping path)

1. VCamProducer starts and calls `registerCamera()`. The service pushes
   `setProducerAvailable(true)` down the frozen AIDL; the HAL reports camera
   100 `PRESENT`; every Camera2 client gets `onCameraAvailable("100")` —
   VCamViewer (which had been waiting) auto-opens it.
2. cameraserver calls the HAL's `configureStreams` (e.g. 3840×2160 RGBA). The
   HAL calls back up: `onStreamsConfigured(w, h, fps)`.
3. `VirtualCameraService` has the JNI pump create a BufferQueue of that shape
   and relays its producer-end Surface to VCamProducer, which starts drawing
   with **OpenGL ES** (`EGL` over the Surface) into RGBA buffers — no CPU
   touches the producer's pixels.
4. Each queued buffer is consumed *by handle only* in system_server and
   forwarded: `queueFrame(NativeHandle, w, h, stride, format, usage, ts)`.
   The vendor HAL clones it into an `AHardwareBuffer`
   (`AHardwareBuffer_createFromHandle`) and keeps just the newest frame.
5. Independently, cameraserver streams capture requests. Per request the HAL's
   **GpuCompositor** imports the producer frame and the framework's output
   buffer as `EGLImage`s and does one GPU blit into the output (§5b). When both
   are RGBA — which the HAL arranges by resolving `IMPLEMENTATION_DEFINED`
   streams to `RGBA_8888` — the blit is a straight passthrough: **zero color
   conversion, and no pixel touches the CPU.** (A consumer that hard-requires
   YUV falls back to a CPU converter; see §5b.)
6. cameraserver hands the filled buffer to VCamViewer's preview. Pixels drawn
   by one app appear in another, through the real camera stack, across the
   Treble boundary, out of an updatable APEX.
7. Producer stops (or dies — binder `linkToDeath`): availability goes false,
   camera 100 turns `NOT_PRESENT`, open clients get `onDisconnected`, the
   viewer returns to "waiting". Measured: add ≈7 ms, remove ≈35 ms.
8. No producer frame yet (registered but idle)? The fill falls back to
   animated SMPTE color bars, so a configured camera is never black.

## 5. The one hard design problem: the Surface relay vs the Treble wall

The HAL wants two things that live on **opposite sides** of the wall:

* **BufferQueue/Surface machinery** → `libgui`, platform-only.
* **Frictionless gralloc** → the mapper and `/dev/dri` access come free in the
  vendor camera-HAL domain; a platform-side HAL needs policy exceptions *and*
  (worse) the mapper initializes once-per-process, so a boot-time denial
  poisons it permanently (§9.1).

You cannot pass the queue itself across the wall: the stable
graphics-bufferqueue interfaces standardize only the **producer** end, and the
queue + consumer implementation is `libgui`. So the resolution is to **split
along the wall exactly where the primitives allow**:

* The **queue and its consumer stay platform-side** (in system_server), where
  `libgui` lives. The producer app's experience is untouched — it just gets a
  Surface.
* What crosses the wall is the thing that *is* stable and cheap to move: the
  **gralloc handle of each consumed frame**, carried by our **frozen VINTF
  AIDL**. Handles travel; pixels do not — the zero-copy guarantee survives the
  boundary.
* Control (availability, stream lifecycle) rides the same frozen interface,
  in the normal Treble direction (system calls down; the HAL answers through
  a registered callback).

Freezing the interface (`stable-aidl/aidl_api/.../1/`) is what makes the APEX
meaningful: platform image and APEX can now rev independently, and any
interface change forces a deliberate new frozen version.

## 5b. The fill: GPU compositing with zero color conversion

Once a producer frame reaches the HAL, the last step is putting its pixels into
the framework's output buffer. Two honest facts shape how:

* **The copy is unavoidable.** The camera framework *owns* the output buffer —
  it's bound to the consumer app's Surface (its ImageReader/SurfaceTexture
  queue). The producer rendered into a *different* buffer. Two buffers, two
  queues → the HAL must move pixels from one to the other. No topology with
  producer ≠ consumer escapes this.
* **The color conversion is avoidable.** If both sides are RGBA, moving pixels
  is a pure blit with nothing to convert.

`core/GpuCompositor` does that move on the GPU and never on the CPU:

1. The producer's `AHardwareBuffer` is imported as a GL texture
   (`eglGetNativeClientBufferANDROID` → `EGLImage` →
   `glEGLImageTargetTexture2DOES`) — zero-copy import.
2. The framework's output buffer is imported as an FBO **renderbuffer**
   (`glEGLImageTargetRenderbufferStorageOES` — the color-renderable path;
   texture attachment is sample-only and fails FBO-completeness).
3. One full-screen shader pass writes the result. **RGBA output → passthrough
   (no conversion at all).** To make this the common case the HAL resolves
   `IMPLEMENTATION_DEFINED` streams to `RGBA_8888` in `configureStreams`, so a
   preview to a `SurfaceTexture` lands on the pure-blit path.

This also **retires the fence hazard**: because the producer now renders with
GL, the frames carry GPU fences, and the compositor's GL pipeline orders
against them — so a GPU producer no longer tears (the old CPU `lockCanvas`
path was only correct because it was synchronous). Validated at steady state:
`0` GPU-composite errors, `0` CPU-fallback calls — no pixel touches the CPU.

**Fallback:** if a consumer hard-requires a YUV stream, `composite()` returns
false and the CPU `FrameFiller` (per-pixel RGB→YUV) fills that buffer. That
path *does* cost a CPU pass; moving it onto the GPU (RGB→NV12 via per-plane
`dma_buf` render targets) is future work — it's driver-dependent and wasn't
needed for the RGBA preview case.

## 6. Shipping as a vendor APEX

### What an APEX is

An **APEX** is Android's updatable package for *system components* — a signed
filesystem image (inside a zip) that `apexd` mounts at `/apex/<name>` early in
boot, before ordinary apps exist. Where an APK ships app code, an APEX ships
native binaries, libraries, init scripts, and config — exactly the payload of
a HAL. A **vendor APEX** lives on `/vendor/apex` and carries vendor HALs
(Cuttlefish itself ships several: wifi, bluetooth, vibrator…).

### Anatomy of ours (`apex/`)

```
com.android.hardware.camera.provider.virtual.apex
├── apex_manifest.json        name + version (the update counter)
├── bin/hw/…provider-virtual-service          the HAL binary
├── lib64/…-virtual-impl.so, …hal-V1-ndk.so   its libraries
├── etc/…virtual.rc           init script (service path is /apex/…/bin/hw/…)
└── etc/vintf/manifest/….xml  declares ICameraProvider/virtual_renderer
                              AND IVirtualCameraHal/default (type="device")
```

Build-side pieces (`apex/Android.bp`):

* **`apex_key` + certificate** — the payload is AVB-signed and the container
  APK-signed (dev keys in-repo; production would rotate them). apexd refuses
  mismatched signatures, which is what makes "update = replace a file" safe.
* **`file_contexts`** — SELinux labels for files *inside* the APEX (our
  binary gets `hal_camera_default_exec`, so init transitions it into the
  standard vendor camera-HAL domain — gralloc, GPU and binder permissions
  included).
* The `.rc` and VINTF fragment ship **inside** the APEX: init picks up
  `/apex/*/etc/*.rc`, and libvintf reads APEX vintf fragments, so the HAL's
  entire lifecycle travels with the package.

### What an APEX **cannot** carry

The platform half stays on the image and follows platform OTAs: the
`VirtualCameraService` code in `services.jar`, the JNI pump on
`/system_ext`, the demo apps (updatable as ordinary APKs anyway), and —
importantly — **SELinux policy**: an APEX may label its own files but cannot
add domains or allow-rules. Policy must be on the image before the APEX can
rely on it.

### The update flow (validated)

```bash
m com.android.hardware.camera.provider.virtual        # 9 s incremental
adb push out/.../vendor/apex/com.android.hardware.camera.provider.virtual.apex /vendor/apex/
adb reboot                                            # ~15 s
adb shell ls /apex | grep provider.virtual            # …provider.virtual@2  ← new version live
```

apexd activates the new version at boot; cameraserver reconnects to the
restarted provider; the platform relay's death-recipient reconnects the
frozen-AIDL session — the pipeline resumes with zero platform changes. (The
manifest also opts into `supportsRebootlessUpdate`; `adb install --staged`
paths vary by release, so the file-replace flow above is the portable one.)

## 7. The integration process

AOSP has no plugin mechanism — integrating means copying source into the
device tree and rebuilding. `scripts/integrate-a13-platform.sh <aosp_root>`
does it all; what it installs and why:

1. **Stable AIDL + platform JNI + APEX config** → `stable-aidl/` (the frozen
   contract), `platform-jni/`, `apex/` into
   `hardware/interfaces/camera/provider/virtual/`.
2. **HAL sources** → `core/` + `aidl/`, with `Android.bp` selected per
   packaging (`platform/bp/` = system_ext relay build,
   `platform/vendor-variant/` = the vendor/APEX build with
   `VCAM_STABLE_AIDL`).
3. **Service + AIDL + apps** → `platform-service/` (compiled into
   `services.jar`), `platform-aidl/`, and the demo apps.
4. **init `.rc` / VINTF fragment** → for the APEX build these ship inside the
   package; loose copies exist for the non-APEX variants.
5. **sepolicy** → into the device policy dir: the domains, service-name
   labels, and app rules (see §11 — the demo currently runs permissive).
6. First build is a full `m`; afterwards, per-module `m` + `adb push`
   (see §8's iterate table).

## 8. Build, run, demo

```bash
./scripts/integrate-a13-platform.sh /path/to/aosp-a13
cd /path/to/aosp-a13 && source build/envsetup.sh \
  && lunch aosp_cf_x86_64_phone-userdebug && m       # first time: full image

launch_cvd --daemon --gpu_mode=gfxstream --cpus=4 --memory_mb=4096
adb root && adb shell setenforce 0                    # demo-mode SELinux (§11)
adb shell setprop ctl.restart vendor.camera-provider-virtual   # fresh mapper init (§9.1)

# order-free: the viewer waits until a producer makes the camera exist
adb shell pm grant com.example.vcamviewer android.permission.CAMERA
adb shell monkey -p com.example.vcamviewer -c android.intent.category.LAUNCHER 1
adb shell am start-foreground-service -n com.example.vcamproducer/.VCamProducerService
# → camera 100 APPEARS, viewer auto-opens, producer frames on screen.
# stop the producer: camera disappears, viewer returns to waiting.
```

Iterating without reflashing — what to push per change:

| You changed | Rebuild | Push | Then |
|---|---|---|---|
| HAL (anything in the APEX) | `m com.android.…provider.virtual` | the `.apex` → `/vendor/apex/` | reboot |
| `VirtualCameraService` / AIDL java | `m services` | `services.jar` → `/system/framework/` **and delete stale AOT**: `/system/framework/oat/*/services.*` + ART apexdata dalvik-cache (§9.10) | reboot |
| JNI pump | `m libvirtualcamera_relay_jni` | the `.so` → `/system_ext/lib64/` (plus `…hal-V1-ndk.so` once) | reboot |
| demo apps | `m VCamProducer VCamViewer` | the APKs → `system_ext/app/...` | reboot |

## 9. Field notes — the bugs you would otherwise hit

1. **Gralloc mapper initialization happens once per process.** minigbm opens
   the DRM device in its constructor at first use. If SELinux denies
   `/dev/dri` at that moment, the mapper stays dead for the process's life —
   `setenforce 0` afterwards does *not* recover it. Relax policy, then
   **restart the HAL** (`ctl.restart …`). (The vendor/APEX packaging avoids
   the problem entirely: `hal_camera_default` already has the access.)
2. **Usage flags must cover every accessor.** The relay's BufferQueue needs
   `SW_READ_OFTEN | SW_WRITE_OFTEN`: the HAL CPU-reads, and a software
   `lockCanvas()` producer CPU-writes. Miss one and `lockCanvas` throws
   `IllegalArgumentException`. A buffer's flags are the union of everyone's
   access pattern.
3. **A mistyped VINTF fragment can wedge boot.** A `type="framework"` fragment
   left in `/vendor/etc/vintf/manifest` corrupts the device manifest →
   servicemanager rejects *every* vendor HAL → keymint fails → keystore2
   hangs → boot never completes. The failure looks totally unrelated; check
   fragment `type=` first after any partition move.
4. **Emulator GPU mode matters.** `guest_swiftshader` (software GL) segfaults
   app HWUI rendering under load; use `gfxstream` (host GPU).
5. **Service-name labels gate registration.** Only the domain that policy
   pairs with a service label may `add` it; a HAL in another domain exits in
   a crash-loop with `avc: denied { add }`. Our two names
   (`…ICameraProvider/virtual_renderer/0`, `…IVirtualCameraHal/default`) need
   labels registerable by `hal_camera_default` (§11).
6. **Stale emulator state mimics real bugs.** Orphaned `crosvm` processes
   (the binary lives at `bin/x86_64-linux-gnu/crosvm` — naive pkill patterns
   miss it) hold instance disk locks and ports (6520/6600/8443), making
   later launches fail in ways that look like kernel/driver issues. Kill by
   exe path, `cvd reset -y`, remove `~/cuttlefish/instances` + `/tmp/cf_*`,
   verify ports free.
7. **Dynamic add/remove is push-driven.** cameraserver only reacts to
   `cameraDeviceStatusChange`, and `getCameraIdList` must agree with the
   pushed state. Availability transitions travel service→HAL over the frozen
   AIDL; the platform side re-syncs after HAL death (APEX update).
8. **Never name an AIDL package segment after a C++ keyword.** Package
   `…camera.virtual` generates `namespace …::virtual` — unbuildable. Watch
   for namespace *shadowing* too: a new `…hardware::virtualcamera` AIDL
   namespace captured the HAL's unqualified `virtualcamera::` references —
   qualify with leading `::`.
9. **R8 strips JNI-only Java methods from services.jar.** Callbacks invoked
   only from native look unused and vanish, aborting `JNI_OnLoad` with
   `NoSuchMethodError`. Give them a Java-visible use (or a keep rule) and
   make JNI `GetStaticMethodID` lookups exception-safe.
10. **A pushed services.jar can be shadowed by stale AOT artifacts** —
    `/system/framework/oat/*/services.{odex,vdex,art}` and the ART apexdata
    dalvik-cache. If new platform code "doesn't run", delete those and reboot.
11. **Vendor code using `AHardwareBuffer_createFromHandle` includes
    `<vndk/hardware_buffer.h>`**, and `libnativewindow` headers want
    `libarect`.

## 10. Repository map

```
stable-aidl/       android.hardware.virtualcamera.hal — the FROZEN V1 boundary
                   (aidl_api/…/1/ is the frozen contract + hash)
platform-jni/      BufferQueue owner + frame pump inside system_server
apex/              vendor APEX packaging: manifest, keys, rc, file_contexts, bp
hal/core/          engine: frame sources (stable push, v1 shm socket, v2 AHB
                   socket, legacy platform relay), GpuCompositor (GLES
                   zero-conversion fill), CPU RGBA→YUV filler fallback (+ color
                   bars), metadata builder
hal/aidl-v1/       Camera AIDL adapter (A13): provider/device/session +
                   VirtualCameraStableHal (the vendor endpoint of the boundary)
hal/aidl-v2/       same adapter for the newer Camera AIDL (A15 path)
service/           VirtualCameraService + VirtualCameraNative (system_server)
platform/          A13 platform half: aidl-lib, services bp, apps
                   (VCamProducer/VCamViewer), bp variants (bp/ = system_ext
                   relay, vendor-variant/ = vendor+stable-AIDL/APEX), sepolicy
scripts/           integrate-a13-platform.sh, build/test/launch helpers
v2-shared-memory/  zero-copy AHardwareBuffer pool + lock-free ring (socket path)
virtual-mic/, virtual-display/   companion virtual devices (same pattern)
unified-test/, sample-renderer/, camera-test/, test-app/   A15-era test apps
```

## 11. Where this goes next

* **Real SELinux policy** (drop the demo's permissive mode): allow
  `hal_camera_default` to register both service names — or dedicated labels —
  plus the `system_server ↔ IVirtualCameraHal` binder rules.
* GPU RGB→YUV (NV12 via per-plane `dma_buf` render targets) so YUV-only
  consumers also avoid the CPU converter (the RGBA preview path is already
  fully GPU / zero-conversion, §5b). Plus 30 fps pacing in the request loop.
* Pass the producer's GPU fence explicitly through `queueFrame` rather than
  relying on GL pipeline ordering.
* Freeze V2 of the boundary when the interface next changes (that's the
  point of it).
* Multi-producer arbitration; virtual mic/display brought up to the same
  APEX + frozen-AIDL pattern.

## License

Apache 2.0 (matching AOSP).
