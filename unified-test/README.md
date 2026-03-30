# Unified Virtual Camera Test App

Single Android app that demonstrates the full virtual camera pipeline:

## Layout

```
┌─────────────────────────────────────────┐
│          Virtual Camera Test            │
├──────────────────┬──────────────────────┤
│  RENDERER VIEW   │    CAMERA VIEW       │
│                  │                      │
│  Vulkan renders  │  Camera2 API shows   │
│  golden cube     │  same cube via HAL   │
│  directly        │                      │
│                  │                      │
│  [60 FPS]        │  [30 FPS]            │
├──────────────────┴──────────────────────┤
│  Status: Renderer active | Camera open  │
│  Frame: 12345    | Latency: 2ms         │
└─────────────────────────────────────────┘
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        Test App                              │
│                                                              │
│  ┌─────────────────┐      ┌─────────────────┐               │
│  │ VulkanRenderer  │      │  Camera2Client  │               │
│  │                 │      │                 │               │
│  │ - Render cube   │      │ - Open cam 100  │               │
│  │ - Copy to shm   │      │ - Preview to    │               │
│  │ - Display left  │      │   right surface │               │
│  └────────┬────────┘      └────────┬────────┘               │
│           │                        │                         │
│           ▼                        │                         │
│  ┌─────────────────┐              │                         │
│  │ Shared Memory   │              │                         │
│  │ /data/local/tmp │              │                         │
│  │ /virtual_camera │              │                         │
│  │ _shm            │              │                         │
│  └────────┬────────┘              │                         │
│           │                        │                         │
└───────────┼────────────────────────┼─────────────────────────┘
            │                        │
            ▼                        │
┌───────────────────────┐           │
│  Virtual Camera HAL   │           │
│                       │◄──────────┘
│  - Read from shm      │   Camera2 API
│  - Convert RGBA→YUV   │   requests frames
│  - Return to Camera2  │
└───────────────────────┘
```

## Components

### 1. VulkanCubeRenderer
- Renders rotating golden cube
- Dual output:
  - Direct to left SurfaceView (preview)
  - Copy to shared memory (for HAL)

### 2. VirtualCameraWriter  
- Creates shared memory file
- Writes RGBA frames with header
- Signals new frame via atomic flags

### 3. Camera2Client
- Opens camera ID 100 (virtual camera)
- Creates preview session
- Displays to right SurfaceView

### 4. LatencyMeter
- Embeds frame number in rendered image
- Reads frame number from camera output
- Calculates pipeline latency

## Building

```bash
cd unified-test
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

## Testing

1. Install app on Cuttlefish
2. App starts rendering and opens camera
3. Both views should show the same cube
4. Rotation should be in sync (minus latency)
5. Status bar shows frame counts and latency

## Success Criteria

- [ ] Left view shows cube (Vulkan direct)
- [ ] Right view shows same cube (via Camera HAL)
- [ ] Cubes rotate in sync
- [ ] Latency < 50ms (ideally < 16ms)
- [ ] No visual artifacts or tearing
