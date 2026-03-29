# AOSP Virtual Camera System

A custom virtual camera HAL for Android that allows apps to act as camera "renderers" — providing frames that appear as a standard camera source to other apps.

## Status: POC Working ✅

The HAL successfully:
- Registers with Android's ServiceManager
- Enumerates as a camera device (shows as 4th camera)
- Creates device and session interfaces on demand
- Responds to camera framework callbacks

**Not yet implemented:**
- Actual frame output (buffers returned as-is)
- Connection to renderer apps
- FMQ metadata queues

## Architecture

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  Camera App     │────▶│  Camera Service  │────▶│  Virtual Camera │
│  (Consumer)     │     │  (Framework)     │     │  HAL Provider   │
└─────────────────┘     └──────────────────┘     └────────┬────────┘
                                                          │
                                                          ▼
                                                 ┌─────────────────┐
                                                 │  Renderer App   │
                                                 │  (Producer)     │
                                                 └─────────────────┘
```

## Quick Start

### Build

```bash
# Copy HAL to AOSP
cp -r hal/* $AOSP/hardware/interfaces/camera/provider/virtual/

# Build
cd $AOSP
source build/envsetup.sh
lunch aosp_cf_x86_64_phone-ap3a-userdebug
m android.hardware.camera.provider-virtual-service
```

### Verify

```bash
# Check HAL is running
adb shell ps -A | grep virtual-service

# Check camera enumeration
adb shell dumpsys media.camera | grep "Number of camera"
# Expected: Number of camera devices: 4

# Check our provider
adb shell dumpsys media.camera | grep virtual_renderer
```

## Key Learnings

### Device Name Format
```cpp
// WRONG: "virtual0"
// RIGHT: "device@1.0/virtual_renderer/100"
```

Camera device names must follow `device@<major>.<minor>/<type>/<id>` format.

### Unique Camera IDs
The numeric ID (e.g., `100`) must be unique across ALL camera providers. Internal cameras use 0, 1, 2.

### Reserved Names
`ICameraProvider/virtual/0` is reserved by AOSP's built-in virtual camera. Use unique instance names.

### SELinux
HAL runs in `hal_camera_default` domain. Service must be labeled:
- Binary: `hal_camera_default_exec:s0`
- Service: `hal_camera_service:s0`

## Directory Structure

```
virtual-camera/
├── hal/                    # HAL source files
│   ├── Android.bp
│   ├── service.cpp
│   ├── VirtualCameraProvider.*
│   ├── VirtualCameraDevice.*
│   ├── VirtualCameraSession.*
│   └── *.rc, *.xml
├── sepolicy/               # SELinux policy fragments
├── sample-renderer/        # Example renderer app (WIP)
├── INTEGRATION.md          # Detailed integration guide
└── README.md               # This file
```

## Next Steps

1. **APEX Packaging** — Package as updateable module
2. **Frame Output** — Implement test pattern generation
3. **Renderer Connection** — Hook up to external renderer apps
4. **BufferQueue** — Proper Surface-based frame passing

## Requirements

- Android 15+ (AOSP)
- AIDL Camera HAL (v1+)
- Target: Cuttlefish or physical device

## License

Apache 2.0
