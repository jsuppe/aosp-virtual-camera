# AOSP Virtual Camera System

A custom virtual camera HAL for Android that allows apps to act as camera "renderers" — providing frames that appear as a standard camera source to other apps.

## Status: Virtual Camera Fully Working! ✅

**Virtual Camera HAL:**
- Registers with Android's ServiceManager
- Enumerates as camera device `100` (4th camera)
- Supports IMPLEMENTATION_DEFINED format (SurfaceView preview)
- Generates animated color bar test patterns
- **Tested: 9,330+ frames @ 30fps** ✅

**Also in this repo:**
- 🎤 **Virtual Microphone** — HAL scaffolding complete (see `virtual-mic/`)
- 🖥️ **Virtual Display** — HWC scaffolding complete (see `virtual-display/`)
- 📄 **Unified API** — Renderer-defined characteristics model (see `RENDERER_API.md`)

## Architecture

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  Camera App     │────▶│  Camera Service  │────▶│  Virtual Camera │
│  (Consumer)     │     │  (Framework)     │     │  HAL Provider   │
└─────────────────┘     └──────────────────┘     └────────┬────────┘
                                                          │
                                                 ┌────────┴────────┐
                                                 │  Test Pattern   │
                                                 │  Generator      │
                                                 │  (Built-in)     │
                                                 └─────────────────┘
                                                          │
                                                          ▼
                                                 ┌─────────────────┐
                                                 │  Renderer App   │
                                                 │  (Future)       │
                                                 └─────────────────┘
```

## Quick Start

### Build (Full Image)

```bash
# Copy HAL to AOSP
cp -r hal/* $AOSP/hardware/interfaces/camera/provider/virtual/

# Build full image (required for VNDK deps)
cd $AOSP
source build/envsetup.sh
lunch aosp_cf_x86_64_phone-ap3a-userdebug
m -j10
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

# List available cameras from app
adb shell dumpsys media.camera | grep "Device .* maps"
# Expected: Device 2 maps to "100" (or similar)
```

### Test with Camera App

The test pattern (animated color bars) will be visible when any app opens camera ID `100`.

## Key Technical Details

### Camera Metadata Requirements

The HAL must provide these required metadata entries:
- `ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS`
- `ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS`
- `ANDROID_SCALER_AVAILABLE_STALL_DURATIONS` ⚠️ **Critical!**
- `ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL`
- `ANDROID_REQUEST_AVAILABLE_CAPABILITIES`

Missing `stallDurations` causes `NullPointerException` in camera framework!

### Buffer Filling with HandleImporter

```cpp
// Lock buffer for CPU access
android_ycbcr ycbcr = sHandleImporter.lockYCbCr(
    handle,
    GRALLOC_USAGE_SW_WRITE_OFTEN,
    region);

// Fill YUV planes
fillTestPattern(ycbcr.y, ycbcr.cb, ycbcr.cr, ...);

// Unlock
sHandleImporter.unlock(handle);
```

### Required VNDK Dependencies

```bp
shared_libs: [
    "android.hardware.graphics.mapper@2.0",
    "android.hardware.graphics.mapper@3.0", 
    "android.hardware.graphics.mapper@4.0",
    "libfmq",
    "libgralloctypes",
    "libhidlbase",
],
static_libs: [
    "android.hardware.camera.common-helper",
],
```

### Device Name Format
```cpp
// Format: device@<major>.<minor>/<type>/<id>
// Example: device@1.0/virtual_renderer/100
```

### Unique Camera IDs
The numeric ID (e.g., `100`) must be unique across ALL camera providers. Internal cameras typically use 0, 1, 2.

## Directory Structure

```
virtual-camera/
├── hal/                    # HAL source files (copy to AOSP)
│   ├── Android.bp
│   ├── service.cpp
│   ├── VirtualCameraProvider.*
│   ├── VirtualCameraDevice.*
│   ├── VirtualCameraSession.*
│   └── *.rc, *.xml
├── sepolicy/               # SELinux policy fragments
├── sample-renderer/        # Vulkan golden cube renderer
├── camera-test/            # Test app for virtual camera
├── INTEGRATION.md          # Detailed integration guide
└── README.md               # This file
```

## Sample Renderer

The `sample-renderer/` directory contains a Vulkan-based Android app that renders a rotating golden cube. Currently renders to local SurfaceView; future work will connect it to the HAL.

```bash
cd sample-renderer
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

## Camera Test App

The `camera-test/` directory contains a minimal app that opens camera ID `100` and displays the output.

```bash
cd camera-test
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

## Roadmap

### Virtual Camera ✅
- [x] HAL registration and enumeration
- [x] Camera metadata (including stallDurations)
- [x] Buffer caching
- [x] Test pattern generation with HandleImporter
- [x] IMPLEMENTATION_DEFINED format support
- [x] **Fully working!** (9,330+ frames tested @ 30fps)
- [ ] VirtualCameraService for renderer connection
- [ ] Surface-based frame passing from renderer apps
- [ ] Renderer-defined characteristics (resolution, format, FPS)

### Virtual Microphone 🎤 (Scaffolded)
- [x] HAL scaffolding (IModule, IStreamIn)
- [x] AIDL service interfaces
- [x] Audio policy configuration
- [ ] FMQ audio data transfer
- [ ] System service implementation
- [ ] Renderer SDK

### Virtual Display 🖥️ (Scaffolded)
- [x] Spec with app-controlled model
- [x] HWC HAL scaffolding (IComposer)
- [x] AIDL service interfaces
- [ ] Full HWC implementation
- [ ] Physical display passthrough
- [ ] System-wide reconfiguration support
- [ ] Renderer app frame delivery

### Packaging
- [ ] APEX packaging for all components

## Requirements

- Android 15+ (AOSP)
- AIDL Camera HAL (v1+)
- Full image build (for VNDK dependencies)
- Target: Cuttlefish or physical device

## License

Apache 2.0
