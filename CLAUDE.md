# AOSP Virtual Camera

Virtual camera HAL for AOSP that lets renderer apps provide frames to consumer apps via Camera2 API.

## Project Structure

- `hal/core/` — Shared frame pipeline (FrameFiller, MetadataBuilder, FrameSource, Socket) — no AIDL deps
- `hal/aidl-v1/` — Camera AIDL V1 adapter (Android 13-14): Provider, Device, Session, service
- `hal/aidl-v2/` — Camera AIDL V2 adapter (Android 15+): Provider, Device, Session, service
- `service/` — Java system service (VirtualCameraService)
- `aidl/` — AIDL interfaces
- `v2-shared-memory/` — Zero-copy buffer pool headers (SharedBufferPool, HalInterface, RendererInterface)
- `unified-test/` — Test app (Kotlin + C++ via NDK/CMake)
- `sample-renderer/` — Vulkan golden cube renderer
- `renderer-lib/` — C++/JNI/Kotlin renderer library
- `apex/` — APEX packaging
- `sepolicy/` — SELinux policies
- `scripts/` — Build, integrate, test, launch scripts

## Build & Deploy

- **Local repo:** `~/dev/aosp-virtual-camera`
- **Build server:** melchior (Tailscale SSH)
- **AOSP tree:** `/mnt/micron/aosp` on melchior (owned by `melchior` user)
- **Deploy:** Push to GitHub → `ssh melchior` → `ssh melchior@localhost` → `cd ~/aosp-virtual-camera && git pull && bash scripts/integrate.sh /mnt/micron/aosp`
- **Build:** tmux session `vcam_build` on melchior
- **Lunch target:** `aosp_cf_x86_64_only_phone-trunk_staging-userdebug`

## Conventions

- Always push changes to GitHub after committing
- HAL includes v2 headers via `include_dirs` (not relative paths — soong doesn't allow `..`)
- v2 socket path: `/data/local/tmp/virtual_camera_v2.sock`
- v1 socket path: `/data/local/tmp/virtual_camera.sock`

## A13 Platform-AIDL Mode (branch a13-platform-aidl)

Producer apps register via platform AIDL instead of the Unix socket:

- **AIDL package:** `android.hardware.virtualcamera` (renamed — `virtual` is a C++ keyword, cpp backend)
- **Flow:** producer app → `IVirtualCameraService.registerCamera()` (system_server) →
  consumer opens camera 100 → HAL creates BufferQueue, sends Surface via
  `IVirtualCameraManager.notifyStreamsConfigured()` → service relays to producer
  `IVirtualCameraCallback.onStreamsConfigured()` → producer draws → HAL converts
  RGBA→YUV into camera output buffers (`AidlFrameSource`, `VCAM_AIDL_SOURCE` builds only)
- **Why relay design:** A13 has no NDK-AIDL HardwareBuffer/Surface; HAL moved to
  system_ext (coredomain) so it can use libgui — mirrors AOSP V virtual camera
- **A13 tree:** /mnt/micron/aosp-a13, target `aosp_cf_x86_64_phone-userdebug`
- **Integrate:** `scripts/integrate-a13-platform.sh /mnt/micron/aosp-a13 [apex|system_ext]`
  (default `apex` = shipping vendor-APEX HAL + real SELinux policy in
  `platform/sepolicy/vendor/`; `system_ext` = iteration-1 relay prototype)
- **Validate:** boot cuttlefish → `scripts/test-a13-platform.sh` (starts VCamProducer
  service, launches VCamViewer, checks frame counters at all 3 stages)
- **Boundary version:** `android.hardware.virtualcamera.hal` is frozen at V2 (fenced
  `queueFrameFenced`); HAL + JNI link `-V2-ndk`, VINTF fragment says version 2. The JNI
  pump in system_server needs `…hal-V2-ndk.so` (built to `system/lib64/`) pushed to
  `/system_ext/lib64/`. Freeze flow: edit `.aidl` → `m <iface>-update-api` →
  `m <iface>-freeze-api` in the tree → copy `stable-aidl/aidl_api/` + `Android.bp` back.
- **Prototype caveats:** the `system_ext` mode runs `setenforce 0` (prototype-grade
  policy); the default `apex` mode runs enforcing with zero denials;
  single static camera id 100 fed by first registered producer; fallback to test
  pattern when no producer registered
