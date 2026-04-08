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
