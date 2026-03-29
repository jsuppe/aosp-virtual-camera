# Virtual Camera HAL - AOSP Integration Guide

## Required Changes (All 4 Must Be Applied)

### 1. VINTF Manifest (`vintf_fragments`)

The HAL must declare itself in a VINTF manifest. The service name **must match exactly**.

**File:** `android.hardware.camera.provider-virtual-service.xml`
```xml
<manifest version="1.0" type="device">
    <hal format="aidl">
        <name>android.hardware.camera.provider</name>
        <version>2</version>
        <fqname>ICameraProvider/virtual_renderer/0</fqname>
    </hal>
</manifest>
```

**Location in source:** `hardware/interfaces/camera/provider/virtual/`

**Android.bp reference:**
```bp
cc_binary {
    name: "android.hardware.camera.provider-virtual-service",
    vintf_fragments: ["android.hardware.camera.provider-virtual-service.xml"],
    ...
}
```

---

### 2. SELinux Service Contexts

Maps the service name to a SELinux type. Required for ServiceManager registration.

**Add to:** `device/<vendor>/<device>/sepolicy/vendor/service_contexts`
```
android.hardware.camera.provider.ICameraProvider/virtual_renderer/0 u:object_r:hal_camera_service:s0
```

**Why `hal_camera_service`:** This type is already granted to `hal_camera` attribute via `hal_attribute_service()` macro.

---

### 3. SELinux File Contexts

Labels the binary so init can transition it to the correct domain.

**Add to:** `device/<vendor>/<device>/sepolicy/vendor/file_contexts`
```
/vendor/bin/hw/android\.hardware\.camera\.provider-virtual-service u:object_r:hal_camera_default_exec:s0
```

**Why `hal_camera_default_exec`:** Uses the existing camera HAL domain. No need to create a new domain.

---

### 4. Init RC Service Definition

Tells init how to start the service.

**File:** `android.hardware.camera.provider-virtual-service.rc`
```rc
service vendor.camera.provider-virtual /vendor/bin/hw/android.hardware.camera.provider-virtual-service
    class hal
    user cameraserver
    group audio camera input drmrpc
    ioprio rt 4
    capabilities SYS_NICE
    task_profiles CameraServiceCapacity MaxPerformance
```

**Android.bp reference:**
```bp
cc_binary {
    name: "android.hardware.camera.provider-virtual-service",
    init_rc: ["android.hardware.camera.provider-virtual-service.rc"],
    ...
}
```

---

## Integration Script

For Cuttlefish/AOSP, run the integration script:

```bash
cd /path/to/aosp
./integrate-virtual-camera.sh
```

The script:
1. Copies HAL source to `hardware/interfaces/camera/provider/virtual/`
2. Adds `file_contexts` entry to device sepolicy
3. Adds `service_contexts` entry to device sepolicy
4. Adds HAL to `PRODUCT_PACKAGES`
5. Rebuilds affected modules

---

## Verification

After building and booting:

```bash
# Check HAL is running
adb shell ps -A | grep virtual

# Check service is registered
adb shell service list | grep virtual_renderer

# Check camera framework sees the provider
adb shell dumpsys media.camera | grep virtual_renderer
```

Expected output:
```
android.hardware.camera.provider.ICameraProvider/virtual_renderer/0: []
```

---

## Common Errors

### Error: `Failed to register service: -3`
**Cause:** VINTF manifest doesn't match service name.
**Fix:** Ensure `<fqname>` in XML matches the string passed to `AServiceManager_addService()`.

### Error: `SELinux: avc: denied { add }`
**Cause:** Missing or incorrect service_contexts.
**Fix:** Add service_contexts entry mapping to `hal_camera_service`.

### Error: `incorrect label or no domain transition`
**Cause:** Missing or incorrect file_contexts.
**Fix:** Add file_contexts entry mapping to `hal_camera_default_exec`.

### Error: Service starts but exits immediately
**Cause:** HAL crashes on startup.
**Fix:** Check logcat for crash details: `adb logcat -s VirtualCameraService:*`

---

## Architecture Alignment

| Requirement | Value |
|-------------|-------|
| Service Name | `ICameraProvider/virtual_renderer/0` |
| SELinux Domain | `hal_camera_default` |
| Service Type | `hal_camera_service` |
| Init Class | `hal` |
| User | `cameraserver` |
