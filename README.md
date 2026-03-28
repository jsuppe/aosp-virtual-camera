# Virtual Camera System for AOSP

A system-level virtual camera that allows apps to register as frame renderers.
**Pure AIDL architecture** — no JNI required.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           RENDERER APP                                  │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │  1. Binds to IVirtualCameraService                               │   │
│  │  2. Calls registerCamera() with config                          │   │
│  │  3. Receives surfaces via IVirtualCameraCallback                 │   │
│  │  4. Renders frames (OpenGL/Vulkan/Canvas)                        │   │
│  └─────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────┬────────────────────────────────────┘
                                     │ AIDL (IVirtualCameraService)
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                      VIRTUAL CAMERA SERVICE                             │
│  (system_server process)                                                │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │  Implements:                                                     │   │
│  │  • IVirtualCameraService — for renderer apps                     │   │
│  │  • IVirtualCameraManager — for Camera HAL                        │   │
│  │                                                                  │   │
│  │  Manages: ImageReader per stream (zero-copy HardwareBuffer)      │   │
│  └─────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────┬───────────────────────────────────────────┘
                              │ AIDL (IVirtualCameraManager)
                              │ Binder IPC
                              ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                      VIRTUAL CAMERA HAL                                 │
│  (separate HAL process)                                                 │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │  • Polls IVirtualCameraManager for registered cameras            │   │
│  │  • Implements ICameraProvider, ICameraDevice                     │   │
│  │  • On capture request: acquireBuffer() → returns HardwareBuffer  │   │
│  │  • Reports cameras to CameraService                              │   │
│  └─────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────┬───────────────────────────────────────────┘
                              │ Camera2 HAL Interface
                              ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                        CAMERA CLIENT APP                                │
│  (Zoom, Meet, TikTok, etc.)                                             │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │  Uses standard Camera2 API                                       │   │
│  │  Sees virtual camera as "external" camera                        │   │
│  └─────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

## AIDL Interfaces

### IVirtualCameraService (renderer apps → service)
```
registerCamera(config, callback) → cameraId
unregisterCamera(cameraId)
getRegisteredCameras() → int[]
isCameraInUse(cameraId) → bool
```

### IVirtualCameraCallback (service → renderer apps)
```
onCameraOpened()
onStreamsConfigured(streams[], surfaces[])
onCaptureStarted(frameRate)
onCaptureStopped()
onCameraClosed()
onStillCaptureRequested(surface, captureId)
```

### IVirtualCameraManager (HAL → service)
```
getRegisteredCameraIds() → int[]
getCameraConfig(cameraId) → VirtualCameraConfig
notifyCameraOpened(cameraId)
notifyStreamsConfigured(cameraId, streams[])
notifyCaptureStarted(cameraId, frameRate)
notifyCaptureStopped(cameraId)
notifyCameraClosed(cameraId)
acquireBuffer(cameraId, streamId) → HardwareBuffer
releaseBuffer(cameraId, streamId, buffer)
requestStillCapture(cameraId, captureId) → HardwareBuffer
```

## Files

```
virtual-camera/
├── aidl/
│   ├── IVirtualCameraService.aidl    # App → Service
│   ├── IVirtualCameraCallback.aidl   # Service → App
│   ├── IVirtualCameraManager.aidl    # HAL → Service (NEW)
│   ├── VirtualCameraConfig.aidl
│   └── StreamConfig.aidl
├── service/
│   ├── VirtualCameraService.java     # Implements both interfaces
│   └── VirtualCamera.java            # Per-camera ImageReader management
├── hal/
│   ├── VirtualCameraProvider.h/cpp   # ICameraProvider + IVirtualCameraManager client
│   ├── VirtualCameraDevice.h/cpp     # ICameraDevice + session
│   └── Android.bp
└── README.md
```

## Zero-Copy Buffer Flow

```
1. Renderer draws to Surface
         ↓
2. Surface backed by ImageReader's BufferQueue
         ↓
3. Frame lands in GraphicBuffer (GPU memory)
         ↓
4. HAL calls acquireBuffer() via IVirtualCameraManager
         ↓
5. Service calls ImageReader.acquireLatestImage()
         ↓
6. Image.getHardwareBuffer() returns handle to SAME GraphicBuffer
         ↓
7. HardwareBuffer sent to HAL via Binder (just the handle, not pixels)
         ↓
8. HAL maps buffer, provides to camera framework
         ↓
9. Camera client receives frame

Total copies: 0 (same underlying gralloc buffer throughout)
```

## 4K60 4:4:4 Support

Configure with RGBA_8888 format (32bpp, equivalent to 4:4:4):

```kotlin
val config = VirtualCameraConfig().apply {
    name = "4K Virtual Camera"
    maxWidth = 3840
    maxHeight = 2160
    maxFps = 60
    supportedFormats = intArrayOf(
        PixelFormat.RGBA_8888,  // 4:4:4 (32bpp)
    )
}
```

Bandwidth: 3840 × 2160 × 4 × 60 = **1.99 GB/s** (handled via DMA, not CPU)

## Integration

### 1. AIDL files → frameworks/base/core/java/android/hardware/camera/virtual/

### 2. Service → frameworks/base/services/core/java/com/android/server/camera/virtual/

Register in SystemServer.java:
```java
ServiceManager.addService("virtual_camera", new VirtualCameraService(context));
```

### 3. HAL → hardware/interfaces/camera/provider/virtual/

Add to device makefile:
```makefile
PRODUCT_PACKAGES += android.hardware.camera.provider-virtual-service
```

### 4. SELinux policies (required)
```
# Allow HAL to connect to service
allow hal_camera_virtual system_server:binder call;
binder_use(hal_camera_virtual)
```

## Latency Analysis

| Stage | Latency |
|-------|---------|
| Render to Surface | ~1ms (GPU) |
| BufferQueue acquire | <0.1ms |
| Binder IPC (handle) | ~0.5ms |
| HAL buffer mapping | ~0.1ms |
| **Total** | **~2ms** |

Compare to vsock approach: ~5-10ms (data copy overhead)

## TODO

- [ ] SELinux policy files
- [ ] Service registration in SystemServer
- [ ] VINTF manifest fragments
- [ ] Permission checks (signature|privileged)
- [ ] Hot-plug camera change notifications (vs polling)
- [ ] JPEG encoding for still capture
- [ ] Multiple format negotiation
