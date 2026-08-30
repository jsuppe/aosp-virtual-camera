# Lesson 8 — Shipping as a vendor APEX

**What you'll understand:** what an APEX is, how the whole vendor HAL fits into
one signed file, what such a file *can't* carry, and how "update = replace one
file" actually works.

---

## Why not just put files in the image?

Up to now the HAL is "some binaries and libraries in `/vendor`." That works, but
to *update* it you'd reflash the vendor image. The frozen interface from Lesson 7
bought us the right to update the vendor half independently — an **APEX** is how
you cash that in.

## What an APEX is

An **APEX** (Android Pony EXpress) is Android's package format for **updatable
system components.** Concretely it's a signed filesystem image inside a zip that
`apexd` **mounts at `/apex/<name>`** very early in boot — before ordinary apps
exist. The contrast with an APK is the whole idea:

| | APK | APEX |
|-|-----|------|
| Ships | app code (Dalvik) | native binaries, libraries, init scripts, config |
| Installed to | `/data/app` | mounted at `/apex/<name>` |
| Runs | as an app | as system/vendor components |

A **vendor APEX** lives on `/vendor/apex` and carries **vendor HALs** —
precisely our payload. (Cuttlefish itself ships several: wifi, bluetooth,
vibrator. You can list them: `adb shell ls /vendor/apex`.)

## Anatomy of ours

Our package —
[`apex/`](../../apex/) — builds
`com.android.hardware.camera.provider.virtual.apex`, which contains:

```
├── apex_manifest.json     name + version   ← the version is the update counter
├── bin/hw/…-service        the HAL binary
├── lib64/…-impl.so, …hal-V1-ndk.so   its libraries (incl. the frozen AIDL, L7)
├── etc/…virtual.rc         init script (service path: /apex/…/bin/hw/…)
└── etc/vintf/manifest/….xml   the VINTF fragment (Lesson 5), declaring BOTH
                                ICameraProvider/virtual_renderer AND
                                IVirtualCameraHal/default
```

Three build-side pieces in [`apex/Android.bp`](../../apex/Android.bp) make it a
*trustworthy* unit:

- **`apex_key` + `android_app_certificate`** — the payload is AVB-signed and the
  container is APK-signed. `apexd` refuses a package whose signature doesn't
  match the expected key. **This is what makes "replace the file" safe** — you
  can't swap in an unsigned or wrong-key APEX. (The repo uses dev keys; a real
  product rotates them.)
- **`file_contexts`** — SELinux labels for files *inside* the APEX. Ours labels
  the binary `hal_camera_default_exec`, so `init` transitions the service into
  the vendor camera-HAL domain (Lesson 5) — gralloc, GPU, and binder access
  included, which is why the vendor-side HAL has none of the Lesson 2 gralloc
  pain.
- The **`.rc` and VINTF fragment ship *inside* the APEX** — `init` reads
  `/apex/*/etc/*.rc` and libvintf reads APEX vintf fragments, so the HAL's whole
  lifecycle (how it starts, that it's declared) travels *with* the package.

## What an APEX cannot carry

This boundary is as important as what it *can* carry, and it's why the system is
split the way it is:

- **`system_server` code** — the `VirtualCameraService` and the JNI frame pump
  (Lessons 4, 7) are platform code in `services.jar` / `/system_ext`. They follow
  **platform OTAs**, not the APEX.
- **SELinux policy** — an APEX may label *its own files* but **cannot add domains
  or allow-rules.** Policy must already be on the image before the APEX can rely
  on it. (This is the top real-world "productization" gap; the demo runs
  permissive.)
- **Apps** — the demo producer/viewer are ordinary APKs, updatable as apps.

So the shipping picture is two-speed: the **platform half** rides slow platform
OTAs; the **vendor HAL** — the part most likely to iterate — updates on its own
via the APEX.

## The update flow (the payoff)

Because the interface is frozen (Lesson 7), updating the HAL is genuinely "swap
one file":

```bash
# bump apex_manifest.json version, rebuild (incremental: seconds)
m com.android.hardware.camera.provider.virtual

# replace the one signed file and reboot
adb push …/vendor/apex/com.android.hardware.camera.provider.virtual.apex /vendor/apex/
adb reboot

adb shell ls /apex | grep provider.virtual
#   com.android.hardware.camera.provider.virtual@2      ← new version live
```

At boot `apexd` activates the new version; cameraserver reconnects to the
restarted provider; the platform relay's death-recipient (Lesson 6/7)
reconnects the frozen-AIDL session — and frames flow again, **with zero changes
to the platform image.** In this project that was a ~9-second incremental build
and a ~15-second boot, `@1` → `@2`, pipeline resumed.

## What broke for real

* **A pushed `services.jar` was shadowed by stale AOT.** Updating the *platform*
  half (not the APEX) didn't take effect because Android had precompiled
  `/system/framework/oat/*/services.{odex,vdex,art}` and the ART apexdata
  dalvik-cache still held the old code. New code "didn't run" until those were
  deleted and the device rebooted. *Lesson: when a pushed jar seems ignored,
  suspect the AOT caches.*
* **The VINTF fragment `type=` again.** The APEX's fragment must be
  `type="device"` (it's vendor) — the same one-word trap from Lesson 5, now
  packaged.

## Takeaways

- An **APEX** is a signed, mounted package of *system* components; a **vendor
  APEX** carries vendor HALs and updates independently of the platform image.
- Signing + `file_contexts` + an in-package `.rc`/VINTF fragment make it a
  self-contained, trustworthy unit — which is what makes **"update = replace one
  file"** safe.
- It **cannot** carry `system_server` code or SELinux policy — those stay on the
  image, which is exactly why the design splits platform vs vendor.
- The frozen interface (Lesson 7) is what lets the two update on different
  schedules.

One thing remained a full CPU cost through all of this: the per-pixel color
conversion when filling the camera buffer (Lesson 2). The final lesson removes it
entirely — on the GPU.

[← Lesson 7](07-frozen-boundary.md) · [Lesson 9: GPU compositing, zero conversion →](09-gpu-zero-conversion.md)
