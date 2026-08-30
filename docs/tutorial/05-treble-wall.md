# Lesson 5 — The Treble wall

**What you'll understand:** why Android is split into "platform" and "vendor"
halves that can't freely call each other, how VINTF and SELinux enforce that
split, and why this one constraint dictates the shape of the whole final design.

---

## Why there's a wall at all

Android runs on thousands of devices from hundreds of vendors. Google ships
platform updates; vendors ship the hardware-specific code. **Project Treble**
(Android 8+) made those two independently updatable by drawing a hard line
between them:

| Partition | Holds | Who owns it |
|-----------|-------|-------------|
| `/system`, `/system_ext` | the Android framework, system services | platform (Google/AOSP) |
| `/vendor` | HALs, hardware-specific libraries | the device vendor |

The line is not a suggestion — it's enforced at link time, at service
registration, and by the security policy. Three mechanisms make the wall real,
and each one bit this project during bring-up.

## 1. The link-time wall: libraries don't cross

Vendor code **cannot link platform-internal libraries**, and vice versa. The one
that matters for us: **`libgui`** — the library that implements BufferQueue and
`Surface` (Lesson 3) — is **platform-only.** A vendor HAL cannot create a
BufferQueue or wrap a Surface.

Sit with that, because it's the crux of the entire architecture:

> We *need* gralloc to work smoothly (that's easiest for a **vendor** HAL — it's
> in the right SELinux domain, Lesson 2's `/dev/dri` story). But we *also* need
> BufferQueue/Surface (that's **platform**-only). Those two needs live on
> opposite sides of the wall.

You cannot simply put everything in one place. The final design (Lessons 7–9)
resolves this by **splitting along the wall exactly where the primitives allow**:
the BufferQueue stays platform-side, and only *buffer handles* — which are
stable and cheap — cross to the vendor HAL. This lesson is why that split exists.

## 2. The manifest wall: VINTF

Even if a HAL registers itself (Lesson 1), `servicemanager` won't let clients
use it unless it's **declared** in a **VINTF manifest** — an XML file listing
which HAL interfaces exist on each side. Our fragment:

```xml
<!-- declares the provider so servicemanager accepts its registration -->
<manifest version="1.0" type="device">
    <hal format="aidl">
        <name>android.hardware.camera.provider</name>
        <fqname>ICameraProvider/virtual_renderer/0</fqname>
    </hal>
</manifest>
```

The `type=` attribute is per-side: **`framework`** for system/system_ext,
**`device`** for vendor. This tiny attribute is load-bearing (see "What broke").

## 3. The security wall: SELinux

Every process runs in an SELinux **domain**; every file, device, and *service
name* has a **label**; policy enumerates which domain may do what to which label.
For us it gates three things:

- opening the GPU device (`/dev/dri`) that gralloc's mapper needs (Lesson 2);
- **registering a service name** — only the domain paired with a service's label
  may `add` it;
- app access to the camera.

A vendor camera HAL that lands in the domain `hal_camera_default` gets gralloc,
GPU, and binder access "for free" — which is a big reason the final HAL ships as
a **vendor** component.

## Reading the split in the repo

The build files encode which side each module lives on. Compare the two variants
kept in the tree:

- Platform build (`platform/bp/…`): modules are `system_ext_specific: true` and
  *can* link `libgui`/`libbinder` — so the HAL could own the BufferQueue, but
  gralloc needs SELinux help.
- Vendor build (`platform/vendor-variant/…`): modules are `vendor: true`, land in
  `hal_camera_default`, get gralloc for free — but **cannot** link `libgui`, so
  the Surface relay is compiled out and something else must move frames.

That "something else" is the frozen AIDL boundary of Lesson 7. The
[integration script](../../scripts/integrate-a13-platform.sh) installs the HAL
sources, the VINTF fragment, an init `.rc`, and the SELinux policy into a device
tree — you can read it as the concrete list of "what has to be placed where" to
satisfy all three walls.

## What broke for real

* **A mistyped VINTF fragment wedged the entire boot.** When the HAL was moved to
  `/vendor`, its manifest fragment still said `type="framework"`. A
  framework-typed fragment sitting in the *device* manifest corrupted it, so
  `servicemanager` rejected **every** vendor HAL — including keymint, so
  keystore2 never started, so boot hung forever with cryptic "waited one second
  for keystore2" spam. The actual cause (one wrong word in an XML file) looked
  nothing like the symptom. *Lesson: after any partition move, check the fragment
  `type=` first.*
* **`avc: denied { add }` crash-loop.** The provider service name was labeled for
  one domain, but the HAL was running in another (`hal_camera_default`) — so it
  couldn't register its own service and exited 255 repeatedly. The service *name*
  label and the process *domain* have to be paired in policy. *Lesson: "who may
  register this name" is a distinct SELinux question from "who may run this
  binary."*
* **`Driver is uninitialized`, revisited.** This is the Lesson 2 gralloc failure
  seen through the Treble lens: it happened *because* the HAL ran in a
  platform-side process without the vendor gralloc access. Moving it to a vendor
  domain made it vanish. The wall wasn't just an obstacle — being on the *right*
  side of it was the fix.

## Takeaways

- Treble splits Android into **platform** (`/system`, `/system_ext`) and
  **vendor** (`/vendor`), independently updatable, separated by a hard wall.
- The wall is enforced three ways: **link-time** (`libgui` is platform-only),
  **VINTF** (a HAL must be declared, with the right `type=`), and **SELinux**
  (domains, service-name labels, device access).
- Our two core needs — smooth gralloc (vendor) and BufferQueue/Surface
  (platform) — sit on **opposite sides**, which forces the split architecture of
  Lessons 7–9.

Before we build that boundary, one more piece of behavior to get right: the
camera should *exist only while a producer is feeding it*. That's dynamic
availability.

[← Lesson 4](04-binder-control-plane.md) · [Lesson 6: Dynamic availability →](06-dynamic-availability.md)
