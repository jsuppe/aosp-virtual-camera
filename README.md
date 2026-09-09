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

> **New to this stack?** There's a companion **[step-by-step tutorial](docs/tutorial/00-index.md)**
> that teaches these concepts one at a time — camera HAL, gralloc, BufferQueue,
> Binder, Treble, dynamic availability, frozen AIDL, APEX, and GPU compositing —
> each building toward this system. This README is the reference; the tutorial is
> the on-ramp.

**Status:** validated end-to-end on Android 13 (Cuttlefish, host-GPU mode) at
4K30 with producer/boundary/viewer frame counters in lockstep, including a
v1→v2 APEX update cycle. The producer renders with **OpenGL ES** and the HAL
composites on the **GPU with zero color conversion** on the RGBA path (steady
state: no pixel touches the CPU — §5b). The device runs **SELinux enforcing**
with real policy for the HAL's two service names and the system_server↔HAL
binder path (§9.15).

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
| **stable-aidl/** `android.hardware.virtualcamera.hal` | build-time (frozen V1 **and V2**) | The Treble contract: `setProducerAvailable`, `queueFrame(NativeHandle+desc)` (V1), `queueFrameFenced(… + acquire fence) → release fence` (V2), `IVirtualCameraHalCallback.onStreamsConfigured/onCameraClosed` |
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
   forwarded: `queueFrameFenced(NativeHandle, w, h, stride, format, usage,
   ts, acquireFence)`. The producer's GPU-done fence travels with the handle
   (nobody CPU-waits on it). The vendor HAL clones the handle into an
   `AHardwareBuffer` (`AHardwareBuffer_createFromHandle`) and keeps just the
   newest frame; the call returns the HAL's read-done fence for the frame it
   just *retired*, which the pump attaches as that buffer's BufferQueue
   release fence — so the producer's next render into it waits for the HAL's
   GPU read (§5c).
5. Independently, cameraserver streams capture requests, and the HAL **paces
   them** to the request's `AE_TARGET_FPS_RANGE` (default 30 fps; the paced
   slot is the frame's sensor timestamp). Per request the HAL's
   **GpuCompositor** imports the producer frame and the framework's output
   buffer as `EGLImage`s, tells the GPU to wait on the acquire fence, and does
   one GPU blit into the output (§5b). When both are RGBA — which the HAL
   arranges by resolving `IMPLEMENTATION_DEFINED` streams to `RGBA_8888` — the
   blit is a straight passthrough: **zero color conversion, and no pixel
   touches the CPU.** The blit's completion fence goes out as the output
   buffer's release fence (the framework waits on it, not the HAL) and back to
   the platform as described in step 4. (A consumer that hard-requires YUV
   falls back to a CPU converter; see §5b.)
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

Once a producer frame reaches the HAL, the last step is getting its pixels into
the framework's output buffer. Two facts shape how — and it's worth being
precise, because "zero-copy" gets overclaimed here:

* **The copy is unavoidable.** The camera framework *owns* the output buffer;
  it belongs to the consumer app's Surface (its ImageReader/SurfaceTexture
  queue) and the framework hands the HAL that specific buffer to fill. The
  producer rendered into a *different* buffer (our relay queue's). Two buffers
  in two different queues → the HAL must move pixels from one to the other. No
  topology with producer ≠ consumer escapes this one move.
* **The color conversion is avoidable.** *Moving* pixels and *converting* them
  are separate costs. If both buffers are RGBA, the move is a plain blit with
  nothing to convert. The producer already renders RGBA (§4); the trick is
  getting the *output* buffer to be RGBA too — see the format nudge below.

So the honest target is: **one GPU blit, no color conversion, and no CPU
touch** — not "zero copy." `core/GpuCompositor` hits it. To follow how, three
GL concepts, each introduced as gralloc was in §2.

**EGLImage — the same buffer, seen by the GPU.** §2 explained that a gralloc
buffer is shared cross-process by handle. An **`EGLImage`** is the GPU's
equivalent handle: a lightweight wrapper that lets a GL context *point at* an
existing gralloc buffer without copying it. You build one from an
`AHardwareBuffer` (`eglGetNativeClientBufferANDROID` → `eglCreateImageKHR`) and
then bind it to a GL object. Creating it is cheap; the pixels stay put. This is
the GPU-side version of "handles travel, pixels don't."

**Texture vs renderbuffer — reading vs writing.** A GL object backed by an
EGLImage can be either:
- a **texture**, which a shader *samples* (reads) — this is how we take the
  producer's frame as input;
- a **renderbuffer**, which the GPU *draws into* (writes) — this is how we
  target the framework's output buffer.

The distinction matters and is a classic trap: you *cannot* attach an
imported-gralloc texture as a render target on most drivers — the framebuffer
comes back "incomplete." Imported buffers are only reliably *renderable*
through a renderbuffer (`glEGLImageTargetRenderbufferStorageOES`). Texture for
the source, renderbuffer for the destination.

**FBO — an off-screen canvas.** Normally GL draws to the screen. A
**Framebuffer Object (FBO)** redirects drawing to a buffer you choose — here,
the renderbuffer wrapping the camera's output buffer. Attach it, set the
viewport, draw a full-screen quad whose fragment shader samples the source
texture, and the result lands in the output buffer.

Putting it together, per capture request:

```
producer AHB ──EGLImage──▶ GL texture (sampled) ─┐
                                                  ├─ full-screen shader pass ─▶
output buffer ──EGLImage──▶ renderbuffer ─FBO────┘        (RGBA→RGBA: a copy,
                                                            never a conversion)
```

**The format nudge.** A preview usually asks for `IMPLEMENTATION_DEFINED` —
"HAL, you pick." We pick `RGBA_8888` (in `configureStreams`) and request
`GPU_RENDER_TARGET` buffers. Now the output is RGBA, the source is RGBA, and
the shader pass is a straight passthrough — **zero color conversion.** A
`SurfaceTexture` consumer (like our viewer) samples RGBA happily, so the common
preview case lands entirely on this path.

Validated at steady state: **0 GPU-composite errors, 0 CPU-fallback calls — no
pixel touches the CPU.**

## 5c. Fences and pacing: making the ordering explicit

A GPU producer finishes asynchronously, and the HAL's GPU read of the frame is
asynchronous too. Two hazards follow from "keep the newest frame":

* **read-before-write** — the HAL samples a buffer the producer's GPU hasn't
  finished drawing;
* **write-before-read** — the platform returns a buffer to the BufferQueue,
  the producer re-dequeues it and starts drawing while the HAL's GPU is still
  reading it.

The first version leaned on the driver's pipeline ordering for the first
hazard (true within one GL context, and only accidentally so across two
processes and a Treble boundary) and simply had the second one, masked by
`glFinish()` in the compositor. V2 of the boundary makes both explicit with
ordinary Android sync fences, and in doing so removes every CPU wait on the
GPU:

| Where | Before | Now |
|---|---|---|
| JNI pump acquires a buffer | `acquireBuffer(waitForFence=true)` — CPU waits for the producer's GPU | acquires *without* waiting; the fence rides down in `queueFrameFenced` |
| HAL samples the frame | trusts pipeline order | `eglWaitSyncKHR` on the acquire fence — the GPU waits, the CPU doesn't |
| Compositor finishes the blit | `glFinish()` on a binder thread | creates an `EGL_SYNC_NATIVE_FENCE_ANDROID`, `glFlush`, `eglDupNativeFenceFDANDROID` → one fd |
| Framework consumes the output | already complete | that fd is the output `StreamBuffer.releaseFence`; the framework waits |
| Producer reuses the source buffer | released unfenced right after `queueFrame` | the pump **holds** the buffer the HAL has; when the next frame retires it, the HAL returns the merged read fence and the pump releases the buffer *with* it — the producer's `dequeueBuffer` fence does the waiting |

The HAL merges fences with `sync_merge` when several capture requests read the
same frame, and `queueFrameFenced` waits (bounded) for an in-flight composite of
the frame it is retiring to submit, so the returned fence is always complete.
The compositor detects `EGL_ANDROID_native_fence_sync`; without it, it falls
back to the old `glFinish()` and returns no fence, which every consumer treats
as "already complete".

**Pacing.** The request loop used to free-run: cameraserver cycles buffers as
fast as the HAL completes them, and with a GPU blit that meant thousands of
frames per second of the *same* producer frame. The session now reads
`ANDROID_CONTROL_AE_TARGET_FPS_RANGE` from the request settings (default 30),
sleeps to the next slot before filling, and reports the slot as the sensor
timestamp plus `SENSOR_FRAME_DURATION`. Measured: viewer receives 150 frames
per 5 s at 4K — 30 fps, in lockstep with the producer.

**Fallback (honest).** If a consumer hard-requires a YUV stream, `composite()`
returns false and the CPU `FrameFiller` does a per-pixel RGB→YUV fill of that
buffer. That path *does* cost a CPU pass. Moving it onto the GPU (RGB→NV12 by
binding the Y and UV planes as separate render targets via
`EGL_EXT_image_dma_buf_import`) is future work — it's driver-dependent and
wasn't needed for the RGBA preview case that the demo exercises.

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
├── lib64/…-virtual-impl.so, …hal-V2-ndk.so   its libraries
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
5. **sepolicy** → into the device policy dirs. Vendor side
   (`platform/sepolicy/vendor/`): the `hal_virtualcamera_service` label for
   `IVirtualCameraHal/default`, the provider name labeled `hal_camera_service`,
   and `system_server ↔ hal_camera_default` binder rules. system_ext side: the
   two `VirtualCameraService` names. Pass `system_ext` as the second argument
   for the iteration-1 relay prototype instead (prototype-grade policy).
6. First build is a full `m`; afterwards, per-module `m` + `adb push`
   (see §8's iterate table).

## 8. Build, run, demo

```bash
./scripts/integrate-a13-platform.sh /path/to/aosp-a13
cd /path/to/aosp-a13 && source build/envsetup.sh \
  && lunch aosp_cf_x86_64_phone-userdebug && m       # first time: full image

launch_cvd --daemon --gpu_mode=gfxstream --cpus=4 --memory_mb=4096
adb root                                              # only for pm grant / logs below

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
| SELinux policy | `m selinux_policy` | `vendor_sepolicy.cil` + `vendor_service_contexts` → `/vendor/etc/selinux/`, `system_ext_sepolicy.cil` + `system_ext_service_contexts` + `system_ext_sepolicy_and_mapping.sha256` → `/system_ext/etc/selinux/`, **and** `odm/etc/selinux/precompiled_sepolicy` + its `.sha256` files → `/odm/etc/selinux/` (§9.16) | reboot |
| `VirtualCameraService` / AIDL java | `m services` | `services.jar` → `/system/framework/` **and delete stale AOT**: `/system/framework/oat/*/services.*` + ART apexdata dalvik-cache (§9.10) | reboot |
| JNI pump | `m libvirtualcamera_relay_jni` | the `.so` → `/system_ext/lib64/` (plus `…hal-V2-ndk.so` from `system/lib64/` once) | reboot |
| the frozen interface itself | edit the `.aidl`, then in the tree `m …hal-update-api && m …hal-freeze-api` | copy `stable-aidl/aidl_api/` + `Android.bp` back to the repo; bump `-V<n>-ndk` in the HAL/JNI `Android.bp` and `<version>` in the VINTF fragment | rebuild both halves |
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
   labels registerable by `hal_camera_default` — see 15.
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
12. **Rendering into an imported gralloc buffer needs a *renderbuffer*, not a
    texture.** Attaching an EGLImage-backed `GL_TEXTURE_2D` as an FBO color
    attachment returns `GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT` — imported
    buffers are sample-only as textures. Use
    `glEGLImageTargetRenderbufferStorageOES` and attach the renderbuffer
    (§5b). Symptom we hit: `glCheckFramebufferStatus` → not-complete.
13. **An EGL context is current on one thread at a time — mind the binder
    pool.** The HAL processes capture requests on binder threads, so the
    thread that *initialized* the GL context is usually not the one that
    *uses* it. Symptom: every GL call silently no-ops and
    `glCheckFramebufferStatus` returns `0` (not an enum — "no context"). Fix:
    `eglMakeCurrent(acquire)` at the top of each composite and
    `eglMakeCurrent(…, EGL_NO_CONTEXT)` (release) at the end, serialized by a
    mutex, and release at the end of init too so the first user can acquire.
14. **`EGLImageKHR` and the `KHR`/`OES` entrypoints live in the *ext* headers.**
    Include `<EGL/eglext.h>` / `<GLES2/gl2ext.h>`, and load the functions
    (`eglCreateImageKHR`, `glEGLImageTarget*OES`,
    `eglGetNativeClientBufferANDROID`) via `eglGetProcAddress` rather than
    linking them directly.
15. **Service labels must live on the same side of Treble as the domain that
    registers them.** A vendor domain may only `add` names typed
    `vendor_service` (neverallow in `public/domain.te`), and a label declared
    in system_ext policy is not that — so the shipping HAL crash-looped under
    enforcing while the iteration-1 label for `ICameraProvider/virtual_renderer/0`
    still sat in `system_ext_service_contexts`. Fix: label the provider name
    `hal_camera_service` in *vendor* `service_contexts`, declare
    `hal_virtualcamera_service` (`vendor_service, protected_service`) for
    `IVirtualCameraHal/default`, and grant `system_server` `find` plus
    two-way `binder_call` with `hal_camera_default`. The HAL also needs
    `hal_client_domain(hal_camera_default, hal_configstore)` because EGL
    init queries `ISurfaceFlingerConfigs`. Two labels for one name shadow
    each other, so the integrate script removes the other mode's entries.
    The stable-AIDL build no longer starts the v1/v2 unix-socket sources
    either — they bound under `/data/local/tmp`, which is off-limits (and
    pointless) for a vendor HAL. Result: `getenforce` = Enforcing, zero
    `avc: denied` lines through register → stream → teardown.
16. **Pushing new `.cil` files does nothing while a precompiled policy blob
    still matches.** Cuttlefish ships `/odm/etc/selinux/precompiled_sepolicy`;
    init loads it whenever its `.sha256` sidecars match the platform policy
    hashes, and only falls back to compiling the `.cil` files when they
    don't. Symptom: the pushed CIL contains the allow rule, `dmesg` shows the
    new type name in the denial, yet the kernel rejects the context
    (`echo -n u:object_r:<type>:s0 > /sys/fs/selinux/context` fails). Push
    the rebuilt blob and hashes too — or delete the blob to force a compile.
    `/sys/fs/selinux/access` answers "what does the *loaded* policy say"
    directly, which is faster than guessing.
17. **`@nullable ParcelFileDescriptor` in the A13 NDK backend is a plain
    `ndk::ScopedFileDescriptor`**, with fd `-1` meaning null — not
    `std::optional`. Overriding with the optional form compiles to an
    unrelated overload and the class stays abstract ("non-virtual member
    function marked override hides virtual member function").
18. **Cutting V2 is one build command and one copy.** `m <iface>-update-api`
    refreshes `aidl_api/…/current/`; `m <iface>-freeze-api` creates
    `aidl_api/…/2/` with its `.hash` and appends `version: "2"` to
    `versions_with_info` in the interface's `Android.bp` — copy both back to
    the repo. Clients pick the version with `getInterfaceVersion()`: the pump
    uses `queueFrameFenced` against a V2 HAL and falls back to unfenced
    `queueFrame` (and CPU-waits the acquire fence on the HAL's behalf) against
    a V1 one. The V1 fallback path is compiled and reviewed but has not been
    exercised against a live V1 APEX.

## 10. Repository map

```
stable-aidl/       android.hardware.virtualcamera.hal — the FROZEN boundary
                   (aidl_api/…/1/ unfenced, aidl_api/…/2/ fenced; each with hash)
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

Three of the original items have shipped (real SELinux policy; the fenced V2
boundary; 30 fps pacing). What remains, in the order it should be done and
why:

### Phase A — make it reproducible (do first, small)

| # | Item | Why now | Done when |
|---|---|---|---|
| A1 | **Full image build + clean boot.** Run a full `m`, launch Cuttlefish from the fresh images with no overlay. | Everything validated so far lives in the Cuttlefish overlay on top of July base images. Until it boots from images, nobody else can reproduce it and a stale overlay can mask a missing tree change. | `test-a13-platform.sh` passes on a first boot with an empty `~/cuttlefish/instances`. |
| A2 | **Live V2-platform / V1-APEX mix.** Build the APEX from the pre-V2 commit (642c141), install it under the current platform, run the demo. | The version negotiation (`getInterfaceVersion` → unfenced `queueFrame`, CPU-waited acquire) is compiled and reviewed but never exercised. It is the one claim in §5c without a log line behind it. | Pump logs "HAL implements V1; unfenced" and frames flow; then swap the V2 APEX back in without reboot (§6) and see "fenced". |
| A3 | **Producer timestamp end-to-end.** Carry `queueFrameFenced`'s `timestampNs` into the capture result (vendor tag or `SENSOR_TIMESTAMP` when the consumer opts in) instead of restamping with the paced slot. | Today no latency number is real: the HAL restamps, so producer→viewer latency cannot be measured from the consumer side (a recurring point of confusion). Tiny change, unlocks honest numbers. | The viewer prints producer-to-delivery latency from result metadata. |

### Phase B — cover the consumers that exist (the substantive work)

| # | Item | Why | Done when |
|---|---|---|---|
| B1 | **GPU RGB→YUV (NV12).** Render the producer frame into the framework's YUV output on the GPU: import the `YCbCr_420_888` gralloc buffer per plane (`EGL_EXT_image_dma_buf_import` with `DRM_FORMAT_NV12`, or two EGLImages over the Y and UV planes) and run a two-pass BT.601 shader. Keep the CPU converter as the fallback it already is. | The zero-conversion path covers `SurfaceTexture` previews only. `ImageReader`, `MediaCodec`, ML pipelines — most real consumers — request YUV and today land on the CPU converter at 4K. This is the largest remaining performance item and the most driver-dependent; expect gfxstream to need the two-plane variant. | A YUV `ImageReader` consumer in VCamViewer shows 0 CPU-fallback calls at 4K30. |
| B2 | **Conformance: run the camera provider VTS against camera 100.** `VtsHalCameraProviderTargetTest` (AIDL V1) with a producer registered. | We have exactly one consumer (our own viewer). VTS is the checklist real apps implicitly depend on: metadata completeness, template settings, flush/close ordering, buffer-management corner cases. It will find gaps the demo cannot. | Test list passes or each failure is a documented, deliberate omission. |
| B3 | **Multi-producer.** One virtual camera per registered producer (100, 101, …) instead of "first producer feeds camera 100". Needs a camera id in the boundary → **V3** of the interface (`onStreamsConfigured(cameraId, …)`, `queueFrameFenced(cameraId, …)`), and the provider enumerating N devices. | The service already has a registry keyed by id; the HAL and the boundary do not. This is the second real use of the freeze discipline. | Two producers, two Camera2 devices, each viewer sees its own producer. |

### Phase C — the pattern beyond the camera

| # | Item | Why | Done when |
|---|---|---|---|
| C1 | **Idiomatic SELinux.** Replace the explicit `system_server ↔ hal_camera_default` rules with a `hal_virtualcamera` attribute (`hal_attribute`, `hal_client_domain(system_server, …)`) in a system_ext *public* policy dir. | Functionally equivalent to what ships; this is what an AOSP reviewer asks for. Cosmetic until then. | Same zero-denial run; the vendor `.te` shrinks to `hal_server_domain` + the service type. |
| C2 | **Virtual mic and display on the same pattern.** Vendor APEX + frozen AIDL + platform-owned queue, reusing the fence protocol (audio: FMQ instead of gralloc). | They are scaffolds today (`virtual-mic/`, `virtual-display/`). The camera has proven the shape; the others should not reinvent it. | Each has its own `stable-aidl/`, APEX, policy, and a demo that survives an APEX update. |
| C3 | **Forward-port to Android 15 (`main` branch).** The A15 NDK AIDL can carry `HardwareBuffer` directly, so the boundary loses `NativeHandle`+descriptor and the platform-side JNI pump shrinks. | The A13 branch is the shipping design because A13 was the constraint; the design should be shown on a current platform too, where it gets simpler. | Same demo, same counters, on `aosp_cf_x86_64_only_phone-trunk_staging`. |

### Deliberately not on the list

* **Copy elimination past the framework.** The one remaining copy (HAL output → framework-owned buffer) is structural to Camera2; §2/§4 explain why. Not a goal.
* **Latency below one frame.** Framework-bound (§2). Measure it (A3), don't chase it.

## License

Apache 2.0 (matching AOSP).
