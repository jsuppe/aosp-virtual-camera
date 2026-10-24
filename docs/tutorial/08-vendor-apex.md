# Lesson 8 — Shipping as a vendor APEX

**What you'll understand:** what the package contains, what must already exist
in the base image, and why staging, activation and qualification are separate.

## Package versus complete integration

An APEX is a signed package containing a filesystem image. `apexd` mounts the
active package under `/apex/<name>`; its native components run with their own
init/SELinux identities rather than as an ordinary app.

This project's factory package is installed under `/vendor/apex` and named
`com.android.hardware.camera.provider.virtual`. The [build definition](../../apex/Android.bp)
packages the provider service, its required implementation/interface libraries,
an init rc file and a VINTF fragment. This native vendor APEX does not package
the demo APKs. APEX as a format is not universally limited to native-only code.

It is **not** an installer that adds the whole virtual camera to untouched
Android. The base image must already supply:

- the system_server broker, framework/app AIDL and its startup integration;
- the JNI BufferQueue relay and matching interface libraries;
- SELinux domains/attributes, service labels and necessary access rules;
- target-appropriate VINTF declarations and compatibility-matrix requirements;
- the trusted APEX signing identities and compatible ABI/graphics dependencies.

Read [the A13 integrator](../../scripts/integrate-a13-platform.sh) or
[the Pi/A17 target guide](../../targets/rpi5-a17/README.md) for the actual target.
Copying only `hal/` and `apex/` is not sufficient integration.

## Two signatures and target-specific placement

`apex_key` signs the payload; `android_app_certificate` supplies the container
signature. Updates must satisfy the installed base's trust and version policy.
The repository keys are development keys, not production secrets or production
trust. Provision product signing before release; changing keys on an existing
installation requires a supported trust/migration process.

`file_contexts` labels files inside the package; it does not add arbitrary
SELinux domains or allow-rules to the already-booted system. Those policies belong
in the base image. The A13 reference has Enforcing evidence for earlier matched
cohorts; the tested Pi/A17 base remains permissive. Do not transfer security
qualification between them.

**VINTF discovery differs by target:** the A13 integration installs an additional
loose vendor fragment because its libvintf path needs that fallback. Pi/A17
discovers the APEX fragment and removes the fallback to avoid duplicate instances.
The provider instance is `ICameraProvider/virtual_renderer/0`; the custom transport
is `IVirtualCameraHal/default` under its own package name.

## Build configuration is not an activation guarantee

At the reviewed source revision:

- `apex/Android.bp` declares `vendor: true` and `updatable: false`.
- `apex_manifest.json` has package version **8** and
  `supportsRebootlessUpdate: true`.
- The custom HAL AIDL is **V6**. Package version 8 is not interface V8, nor proof
  that version 8 is installed on a device.

On the tested A13 branch, the supported vendor Soong configuration does not use
`updatable: true`, yet signed staged shell installs were demonstrated on a
matched integrated base. Conversely, the rebootless manifest flag does **not**
establish working non-staged/rebootless activation. The documented reference
flow stages and **reboots**.

## A staged update, with explicit prerequisites

First select the exact device/base, verify the artifact hash, package/version,
signing compatibility and platform/JNI cohort, and check for unrelated pending
staged sessions. Then, on a compatible target, the command shape is:

```sh
adb -s SERIAL install --staged -i com.android.shell \
  --staged-ready-timeout 15000 /absolute/path/to/approved-newer.apex
adb -s SERIAL shell pm list staged-sessions --only-parent
```

Require the expected session to be **READY and not failed** before a planned
reboot. After reboot, verify that same session is **APPLIED**, Android completed
boot, and `/apex/apex-info-list.xml` identifies the expected active version/path.
A data update should be active under `/data/apex/active/`, with its factory
package retained. Then rerun registration, lifecycle and pixel-delivery checks.
Upload success alone is not activation, and activation alone is not correctness.

Do **not** copy a file into `/data/apex/` and assume installation occurred.
Replacing `/vendor/apex/` on a remounted development image is a different
factory-image modification workflow, not the staged-update procedure above.

The repository's [reference installer](../../scripts/install-apex-reference.py)
adds artifact and base-hash checks and requires explicit `--reboot` to reboot:

```sh
python3 scripts/install-apex-reference.py --help
```

It intentionally pins **one older A13 reference cohort** and refuses other
serials/base hashes, including Pi. It is not a generic installer for current V6
source; do not remove its guards to make an unmatched device pass.

The [Pi investigation](../pi5-extensive-20261024/README.md) built a diagnostic
APEX without staging it. Earlier Pi staging failed; neither a package build nor
A13 success qualifies Pi staged activation, rollback or rebootless updates.

## What broke for real

- Updating platform jars without matching build artifacts/AOT state can leave
  stale code. Prefer a coherent image rebuild/OTA; deleting caches is not a
  general deployment recipe.
- Duplicate or incorrectly typed VINTF declarations can prevent HAL discovery
  or boot. Check target placement, not just XML syntax.
- Shipping loose and APEX provider services together can start duplicate HALs.

## Takeaways

- The APEX updates the **vendor component on a compatible integrated base**.
- Service/JNI, policy, trust and compatibility requirements remain base concerns.
- Distinguish build, staging, reboot activation, camera regression and recovery.
- Rebootless flags and frozen AIDL are prerequisites/capabilities, not qualification.

[← Lesson 7](07-frozen-boundary.md) · [Lesson 9: GPU compositing, zero conversion →](09-gpu-zero-conversion.md)
