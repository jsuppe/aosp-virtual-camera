# Virtual Camera v2 — Shared Memory Architecture

**Target: Sub-millisecond latency for 4K60 4:4:4**

## The Problem with v1

```
Every frame:
  Renderer → Binder IPC → Service → Binder IPC → HAL
                 ↑                        ↑
              ~0.5ms                   ~0.5ms
```

Binder = context switches = latency.

## v2 Solution: Shared Buffer Ring

```
SETUP (once, via Binder):
┌──────────────┐         ┌──────────────┐         ┌──────────────┐
│ Renderer     │ ──────▶ │ Service      │ ──────▶ │ HAL          │
│              │  setup  │              │  setup  │              │
└──────────────┘         └──────────────┘         └──────────────┘
       │                                                  │
       │◀─────────── shared HardwareBuffers ─────────────▶│
       │◀─────────── shared control ring ────────────────▶│

RUNTIME (per frame, NO Binder):
┌──────────────┐                              ┌──────────────┐
│ Renderer     │                              │ HAL          │
│              │                              │              │
│ 1. Get free  │   ┌──────────────────────┐   │ 4. Read from │
│    buffer    │   │   Shared Memory      │   │    ring      │
│              │   │                      │   │              │
│ 2. GPU render│   │  ┌────┬────┬────┐   │   │ 5. Wait fence│
│    to buffer │   │  │ B0 │ B1 │ B2 │   │   │              │
│              │   │  └────┴────┴────┘   │   │ 6. Use buffer│
│ 3. Write to  │   │   Buffer Pool       │   │              │
│    ring +    │   │                      │   │ 7. Release   │
│    signal    │──▶│  ┌──────────────┐   │◀──│    buffer    │
│    fence     │   │  │ Control Ring │   │   │              │
│              │   │  │ [W][R][idx]  │   │   │              │
└──────────────┘   │  └──────────────┘   │   └──────────────┘
                   └──────────────────────┘
```

## Components

### 1. Shared Buffer Pool
Pre-allocated HardwareBuffers (3-4 for triple/quad buffering):
```cpp
struct SharedBufferPool {
    AHardwareBuffer* buffers[4];  // dmabuf-backed
    int bufferCount;
    int width, height, format;
};
```

### 2. Control Ring (lock-free)
Shared memory region with atomic read/write indices:
```cpp
struct ControlRing {
    std::atomic<uint32_t> writeIndex;   // Renderer increments
    std::atomic<uint32_t> readIndex;    // HAL increments
    
    struct Slot {
        uint32_t bufferIndex;           // Which buffer has the frame
        int32_t fenceFd;                // Sync fence (-1 if signaled)
        int64_t timestampNs;            // Presentation time
    } slots[16];  // Ring of pending frames
};
```

### 3. Sync Fences
GPU completion signaling without CPU involvement:
```cpp
// Renderer (after GPU submit):
int fenceFd = eglDupNativeFenceFDANDROID(display, syncObj);
ring->slots[writeIdx].fenceFd = fenceFd;

// HAL (before reading):
sync_wait(slot.fenceFd, timeout_ms);
close(slot.fenceFd);
```

## Latency Breakdown

| Stage | Time |
|-------|------|
| Renderer writes to ring | ~10ns (atomic store) |
| HAL reads from ring | ~10ns (atomic load) |
| Fence wait (GPU done) | 0ms (already signaled) or GPU time |
| **Total overhead** | **<1µs** |

Actual latency = **GPU render time only**

## Implementation Files

```
v2-shared-memory/
├── SharedBufferPool.h    # Buffer pool + lock-free control ring + release ring
├── RendererInterface.h   # Renderer-side: EGL images, frame submit
├── HalInterface.h        # HAL-side: frame acquire, fence wait, buffer release
└── README.md

hal/
├── VirtualCameraFrameSourceV2.h   # Socket protocol + pool setup
└── VirtualCameraFrameSourceV2.cpp # Receives AHardwareBuffers, wires HalInterface
```

## Setup Flow (Binder, once)

```cpp
// 1. Service creates pool
SharedBufferPool pool;
pool.allocate(3840, 2160, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 4);

// 2. Service sends FDs to HAL via Binder
int controlFd = pool.getControlRingFd();
int bufferFds[4];
for (int i = 0; i < 4; i++) {
    bufferFds[i] = pool.getBufferFd(i);
}
// Send via IVirtualCameraManager.setupSharedBuffers(controlFd, bufferFds, config)

// 3. HAL attaches to same pool
SharedBufferPool halPool;
halPool.attach(controlFd, bufferFds, 4, config);

// 4. Renderer attaches + creates EGL resources
SharedBufferPool rendererPool;
rendererPool.attach(controlFd, bufferFds, 4, config);
RendererInterface renderer;
renderer.initialize(&rendererPool, eglDisplay);
```

## Runtime Flow (no Binder, per frame)

```cpp
// RENDERER (every frame):
Frame frame = renderer.beginFrame();       // ~10ns - get free buffer
if (frame.valid()) {
    glBindFramebuffer(GL_FRAMEBUFFER, frame.framebuffer);
    // ... your rendering code ...
    renderer.submitFrame(frame);           // ~100ns - atomic write + fence
}

// HAL (on capture request):
AcquiredFrame frame = hal.acquireLatestFrame(16);  // ~10ns if ready
if (frame.valid()) {
    // frame.buffer is the rendered HardwareBuffer
    // Map it, use it, etc.
    hal.releaseFrame(frame);               // ~10ns
}
```

## Comparison

| Approach | Per-frame overhead | Context switches |
|----------|-------------------|------------------|
| v1 (Binder) | ~2ms | 4 (R→S→H→S→R) |
| v2 (Shared mem) | ~1µs | 0 |
| **Improvement** | **2000x** | **∞** |

## Requirements

- `AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT` - render target
- `AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE` - HAL can read
- `EGL_ANDROID_native_fence_sync` - GPU sync fences
- `ashmem` - shared memory for control ring

## Caveats

1. **Buffer release notification**: ✅ Solved via release ring embedded in ControlRing.
   HAL calls `tryWriteRelease(bufferIndex)`, renderer drains on `beginFrame()`.

2. **Error handling**: If renderer crashes, HAL needs to detect and recover.

3. **Format negotiation**: Should happen at setup time via Binder.

## Complete Latency (4K60 4:4:4)

```
Frame N rendered:
  GPU render time:     1-5ms (depends on content)
  Submit to ring:      ~100ns
  
Frame N consumed by HAL:
  Ring read:           ~10ns
  Fence wait:          0ms (GPU already done)
  Buffer access:       0ms (already mapped)
  
Total added latency:   <1µs
Effective latency:     GPU render time only
```

For a passthrough/copy operation: **~0.1ms**
For complex rendering: **1-5ms** (your GPU work)
