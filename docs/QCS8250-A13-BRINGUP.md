# QCS8250 / Android 13: first device test release

Target confirmed by Jon on October 16, 2026: a remote QCS8250 device running
Android 13. The TCL tablet attached to the development host is not the target.

## Current readiness

- The A13/x86_64 Cuttlefish reference APEX rebuilt successfully on October 16.
- Package name: `com.android.hardware.camera.provider.virtual`; package version 2.
- SHA-256: `f8272b37e70de95bc4126c89b01fb0bf4ff0da5d9f868c93ed4a1329c3e299bf`.
- This is NOT an ARM64/QCS8250 artifact and must not be installed on that device.
- No hardware access is needed to start a target build. The BSP source location,
  exact product/lunch target and image integration are the next dependencies.

## 1. Establish the target build contract

Obtain the QCS8250 A13 BSP/AOSP tree location and exact lunch target, including
the host if remote. Record its source revision/manifest, product device,
CPU ABIs, build type, VNDK/vendor API configuration, existing camera-provider
instances/IDs, graphics mapper/allocator versions, and APEX support.

Confirm whether this image already has VirtualCameraService, the JNI relay,
custom-HAL declarations and policy. If absent, the first release needs an
integrated base image, not just a new APEX. Confirm the team's normal test-image
installation and recovery process before deployment; do not unlock or erase a
device as part of an automatic APEX install.

The device can remain at its location: its operator can collect read-only
information, or we can use an authorized remote ADB connection later. Do not
assume the QCS8250 vendor stack matches Cuttlefish/minigbm/gfxstream.

## 2. Prepare a BSP-specific integration patch

Use branch `a13-platform-aidl` as the reference. Keep the HAL engine and frozen
`android.hardware.virtualcamera.hal` V3 interface, including older-interface
negotiation. Integrate the following in the QCS product's actual paths:

1. HAL core, A13 adapter, stable AIDL, APEX modules and signing configuration.
2. Platform service in services.jar and its SystemServer startup hook.
3. ARM64 platform JNI relay, installed to system_ext/lib64.
4. Vendor provider/custom-HAL VINTF declarations plus framework compatibility
   matrix. A13 needs the vendor-image VINTF fragment outside the APEX.
5. Platform-public HAL attribute, platform-client and vendor-server policy,
   Binder service labels and APEX file labels.
6. Product package inclusion for the APEX, JNI relay, VINTF prebuilt, and demo
   producer/viewer APKs. Avoid installing/starting a second loose provider.

Do not run `integrate-a13-platform.sh` unchanged on the Qualcomm BSP: it edits
Cuttlefish-specific product/policy files and replaces integration directories.
Port the integration deliberately and preserve vendor/product customizations.

Initially retain the existing device's physical camera HAL. Allocate virtual
camera IDs without colliding with it. Choose the signing keys accepted by the
test base image; a package signed with unrelated keys is not a valid update.

## 3. Build the ARM64 test bundle

In the QCS BSP, select its real product target, then build:

```bash
source build/envsetup.sh
lunch <confirmed-QCS8250-A13-product-target>
m -j8 com.android.hardware.camera.provider.virtual \
    libvirtualcamera_relay_jni services \
    android.hardware.camera.provider-virtual-service.vintf \
    VCamProducer VCamProducer2 VCamViewer
```

The lunch argument is intentionally not guessed. Build the required product
images and policy using the BSP's documented image/OTA targets after integration;
building these individual modules alone does not produce a bootable base image.

Before handing off, inspect package metadata and contained ELF architecture
(must be AArch64), dependency availability/linker namespace behavior, policy and
VINTF checks, signatures and source provenance. Package a manifest with the
target fingerprint/cohort, source revision, exact build target, APEX version,
hashes and installation/recovery instructions. Mark it internal-test-only.

For the initial image, use a matched base+APEX bundle. For later APEX-only
updates, keep the platform/policy/interface contract compatible and use a new
package version. Do not assume supportsRebootlessUpdate establishes a working
PackageInstaller or rebootless update path; the reference currently uses vendor
replacement plus reboot on userdebug systems.

## 4. Read-only checks on the remote device

Once the ARM64 artifact and base integration exist, run on its ADB host:

```bash
bash scripts/preflight-apex-device.sh <explicit-device-serial> 33 arm64-v8a
```

The helper checks the existing userdebug reference-update prerequisites. On an
unintegrated device it is expected to report missing components; that means
bootstrap the base image, not force-install the APEX. It performs no root,
remount, install, flash, reboot, settings change or data deletion. Passing it
does not prove signatures, Qualcomm graphics compatibility or frame correctness.

## 5. Validate on QCS8250 before broadening testing

- Clean boot, SELinux enforcing; expected APEX version mounted; one virtual
  provider process; both HAL interfaces and platform broker registered.
- Physical cameras still work, with no ID or service-name collisions.
- Start producer and viewer at 720p30 first. Check moving pixels, timestamps,
  acquire/release fence correctness, no EGL/gralloc import errors or new AVCs.
- Add 1080p30, then 4K only if device measurements support it. Exercise explicit
  YUV and JPEG consumers. Qualcomm layout/format behavior must be measured, not
  inferred from the gfxstream packed-plane implementation.
- Exercise two producers, removal of one producer, producer crash and HAL
  restart. Confirm no stale frames or cross-camera interference.
- Run camera-provider VTS against the actual virtual instance, save test output
  and failure details. Reference 37/37 results are historical Cuttlefish results,
  not QCS certification.
- Install a compatible newer test APEX through the agreed device workflow;
  reboot/reconnect, repeat smoke tests, and verify the documented recovery path.

The known producer-authorization/UID-ownership and fence-timeout/error issues
remain hardening work. Keep early device testing within a controlled internal
environment; do not label this test bundle production-ready.
