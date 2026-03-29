# Virtual Camera Test App

A simple Android app to validate the virtual camera HAL functionality.

## What it tests

1. **Camera enumeration** — Verifies camera ID 100 (virtual camera) is visible
2. **Camera open** — Tests `CameraDevice.open()` on camera 100
3. **Stream configuration** — Tests `createCaptureSession()` with 1280x720 RGBA
4. **Frame delivery** — Captures frames for 5 seconds, counts them
5. **Frame content** — Analyzes first frame for non-zero content
6. **FPS calculation** — Reports actual framerate achieved

## Building

```bash
# From test-app directory
./gradlew assembleDebug

# APK will be at:
# app/build/outputs/apk/debug/app-debug.apk
```

## Installing

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## Running

1. Launch "VCam Test" app
2. Grant camera permission when prompted
3. Press "Start Test"
4. Wait 5 seconds for test to complete
5. Review results

## Expected output (PASS)

```
=== Starting Virtual Camera Test ===
Target: Camera ID 100
Resolution: 1280x720
Duration: 5000ms

Opening camera...
✓ Camera opened successfully
Creating ImageReader 1280x720 RGBA
Creating capture session...
✓ Capture session configured
Starting capture...
✓ First frame received!
Frame analysis:
  Format: 11
  Size: 1280x720
  Pixel[0,0]: RGBA(85, 42, 170, 255)
  ✓ Frame has content!
  Frame 10 (30.2 fps)
  Frame 20 (30.1 fps)
  ...

=== Test Results ===
Total frames: 150
Duration: 4980ms
Average FPS: 30.12
Status: PASS ✓
```

## Troubleshooting

- **Camera 100 not found** — Virtual camera HAL not registered
- **Camera error: CAMERA_DEVICE** — HAL crashed during open
- **No frames captured** — HAL not delivering frames
- **All pixels black** — Test pattern not being written to buffers
