# Virtual Camera APEX

Reviewed 2026-10-24. Start with
[the vendor APEX tutorial](../docs/tutorial/08-vendor-apex.md) for prerequisites,
signing, staged activation and target-specific limitations.

## What this package contains

The vendor provider service, implementation and required interface libraries,
init rc, VINTF fragment and package-local file labels. It does **not** supply the
system_server broker, JNI relay, base SELinux policy or initial signing trust.
Use the [A13 integrator](../scripts/integrate-a13-platform.sh) or
[Pi/A17 target integration](../targets/rpi5-a17/README.md) to prepare a matched
base; copying only this directory and the HAL sources is insufficient.

## Build on an already integrated source tree

```sh
cd /absolute/path/to/matched/aosp
source build/envsetup.sh
lunch TARGET_FOR_MATCHED_TREE
m com.android.hardware.camera.provider.virtual
```

Replace `TARGET_FOR_MATCHED_TREE` with that target's lunch selection.
Preserve the same absolute `OUT_DIR` used for the target build. The factory
output is normally under
`$OUT_DIR/target/product/<device>/vendor/apex/com.android.hardware.camera.provider.virtual.apex`.
The reviewed source has package version 8 and custom HAL interface V6; neither
number tells you which package is active on a connected device.

## Install/update

An initial compatible factory/base integration is required. For a verified,
newer signed artifact on a supported matched target, use the staged-install
procedure in [lesson 8](../docs/tutorial/08-vendor-apex.md#a-staged-update-with-explicit-prerequisites).
Do not copy files into `/data/apex/` as an installation method. Do not treat
upload success as READY/APPLIED status, or activation as camera qualification.

The [reference installer](../scripts/install-apex-reference.py) is restricted to
one pinned older A13 cohort; it intentionally does not support Pi or arbitrary
current builds. `--help` documents its artifact/base checks and explicit reboot
option. Do not bypass its guards.

`updatable: false` in the vendor Soong definition and
`supportsRebootlessUpdate: true` in the manifest are not runtime qualification
results. A matched A13 staged-and-rebooted update was demonstrated; rebootless
activation and Pi staged updates are not established by that result.

## Signing

The checked-in payload/container keys are **development keys**. Production
requires separately provisioned signing/trust and a supported update policy.
Changing a key file does not make an already installed base trust a replacement.

## Read-only inspection

```sh
adb -s SERIAL shell cat /apex/apex-info-list.xml
adb -s SERIAL shell pm list staged-sessions --only-parent
adb -s SERIAL shell service list
adb -s SERIAL shell dumpsys media.camera
```

Inspect the exact active package version/path and staged session status, then
check broker/HAL registration and camera behavior. Keep activation failures and
camera-test failures separate in the evidence.
