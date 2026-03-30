# Virtual Display System

A system where apps render to a virtual display (as default), and a renderer app composites the output to the physical display via Vulkan.

## Key Differentiator: System-Wide Configuration Control

Unlike Virtual Camera and Virtual Mic where the renderer defines device characteristics that **consumer apps see**, the Virtual Display renderer defines characteristics that **ALL apps render to** — changing the entire system's display configuration.

| Aspect | Virtual Camera / Mic | Virtual Display |
|--------|---------------------|-----------------|
| **Renderer controls** | What consumers receive | What ALL apps render to |
| **Scope** | Single consumer at a time | Entire system |
| **System impact** | None | Triggers onConfigurationChanged() for all apps |
| **Resolution change** | Consumer adapts | All apps adapt |

**Virtual Display is uniquely powerful:**
```
Renderer App says: "I want 4K @ 120Hz with HDR10"
        │
        ▼
┌─────────────────────────────────────────────────┐
│  ENTIRE SYSTEM RECONFIGURES                    │
│  - All apps receive onConfigurationChanged()   │
│  - All surfaces reallocate to 4K               │
│  - All rendering now happens at 4K @ 120Hz     │
│  - HDR content is now supported                │
└─────────────────────────────────────────────────┘
        │
        ▼
Renderer receives 4K HDR frames from ALL apps
```

This makes the Virtual Display renderer a **system compositor** with full control over the display pipeline.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              ANDROID APPS                                   │
│                    (render normally to "default" display)                   │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                            SURFACEFLINGER                                   │
│              (composites app surfaces to virtual display)                   │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                     VIRTUAL DISPLAY HAL (HWC)                               │
│   - Appears as primary/default display to SurfaceFlinger                    │
│   - Receives composited frames                                              │
│   - Provides frames to renderer app via Surface/BufferQueue                 │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                          RENDERER APP                                       │
│   - Receives frames from virtual display                                    │
│   - Applies effects/transformations (optional)                              │
│   - Renders to physical display via Vulkan                                  │
│   - Has direct access to physical display (exclusive)                       │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         PHYSICAL DISPLAY                                    │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Components

### 1. Virtual Display HWC (Hardware Composer HAL)

The HWC HAL presents a virtual display as the primary/default display:

```
hardware/interfaces/graphics/composer/
├── VirtualDisplayComposer.cpp   # IComposer implementation
├── VirtualDisplayClient.cpp     # IComposerClient implementation  
└── VirtualDisplay.cpp           # Display management
```

**Key responsibilities:**
- Report virtual display as primary display to SurfaceFlinger
- Accept composited layers from SurfaceFlinger
- Output frames to BufferQueue for renderer app
- Handle display configuration (resolution, refresh rate, HDR, etc.)

### 2. Physical Display Service

A separate service that manages the physical display:

```
service/
├── IPhysicalDisplayService.aidl
└── PhysicalDisplayService.java
```

**Key responsibilities:**
- Initialize and manage physical display hardware
- Provide Surface/window to renderer app
- Handle display power states
- Coordinate with VirtualDisplayHWC

### 3. Renderer App

User-space app that bridges virtual display → physical display:

```kotlin
class DisplayRendererApp {
    // Get frames from virtual display
    val virtualDisplaySurface = virtualDisplayService.getSurface()
    
    // Get surface for physical display output
    val physicalSurface = physicalDisplayService.getSurface()
    
    // Vulkan renderer
    val renderer = VulkanRenderer(physicalSurface)
    
    fun onFrame(frame: HardwareBuffer) {
        // Optional: apply effects
        val processed = applyEffects(frame)
        
        // Render to physical display
        renderer.render(processed)
    }
}
```

## Display Configuration

### Virtual Display Properties

```kotlin
VirtualDisplayConfig.Builder()
    // Resolution (what apps see)
    .setWidth(1920)
    .setHeight(1080)
    
    // Refresh rate
    .setRefreshRate(60.0f)
    .setSupportedRefreshRates(60.0f, 90.0f, 120.0f)
    
    // Density
    .setDensityDpi(420)
    
    // HDR support
    .setHdrCapabilities(HdrCapabilities.HDR10, HdrCapabilities.HLG)
    
    // Color modes
    .setColorModes(ColorMode.NATIVE, ColorMode.SRGB, ColorMode.DISPLAY_P3)
    
    // Flags
    .setFlags(DisplayFlags.SUPPORTS_PROTECTED_BUFFERS)
    
    .build()
```

### Renderer Configuration

```kotlin
RendererConfig.Builder()
    // Effects pipeline
    .addEffect(ColorCorrectionEffect())
    .addEffect(ScalingEffect(ScaleMode.FIT))
    .addEffect(OverlayEffect(overlayTexture))
    
    // Latency target
    .setTargetLatencyMs(8)  // Sub-frame latency
    
    // Vsync alignment
    .setVsyncAligned(true)
    
    .build()
```

## Data Flow

### Frame Path (Low Latency)

```
1. App draws to Surface
2. SurfaceFlinger receives buffer
3. SurfaceFlinger composites all visible apps
4. Composited frame sent to VirtualDisplayHWC
5. VirtualDisplayHWC queues frame to BufferQueue
6. Renderer app dequeues frame
7. Renderer applies effects (GPU)
8. Renderer presents to physical display
```

**Target latency:** <16ms total (one frame at 60fps)

### Buffer Strategy

```
VirtualDisplayHWC                    Renderer App
      │                                    │
      │  ┌─────────────────────────────┐  │
      │  │     BufferQueue (3-4)       │  │
      │  │  ┌───┐ ┌───┐ ┌───┐ ┌───┐   │  │
      ├──│──│ 0 │ │ 1 │ │ 2 │ │ 3 │───│──┤
      │  │  └───┘ └───┘ └───┘ └───┘   │  │
      │  └─────────────────────────────┘  │
      │                                    │
   produce                             consume
```

Triple/quad buffering for smooth frame pacing while allowing renderer processing time.

## HWC HAL Interface

Modern Android uses AIDL HWC HAL (`hardware/interfaces/graphics/composer/aidl/`):

```
IComposer
├── createClient() → IComposerClient
└── getCapabilities()

IComposerClient
├── createVirtualDisplay()
├── destroyVirtualDisplay()
├── createLayer()
├── destroyLayer()
├── getDisplayCapabilities()
├── getDisplayConfigs()
├── setActiveConfig()
├── setClientTarget()
├── setLayerBuffer()
├── validateDisplay()
├── presentDisplay()
└── ...
```

### Key Implementation Points

**1. Primary Display Registration**
```cpp
// In VirtualDisplayComposer initialization
void registerPrimaryDisplay() {
    // Report virtual display as Display ID 0 (primary)
    mDisplays[0] = VirtualDisplay(
        DisplayType::PRIMARY,
        1920, 1080,  // Resolution
        60.0f,       // Refresh rate
        420          // DPI
    );
    
    // Notify SurfaceFlinger
    mCallback->onHotplug(0, Connection::CONNECTED);
}
```

**2. Frame Output to Renderer**
```cpp
ndk::ScopedAStatus VirtualDisplayComposer::presentDisplay(
        int64_t display, PresentFence* outFence) {
    
    // Get composited frame from client target
    buffer_handle_t frame = mClientTarget[display];
    
    // Queue to BufferQueue for renderer app
    mOutputQueue->queueBuffer(frame, ...);
    
    // Return present fence
    outFence->fence = createFence();
    return ndk::ScopedAStatus::ok();
}
```

**3. Physical Display Bypass**
```cpp
// The physical display is NOT registered with SurfaceFlinger
// Renderer app has exclusive access via separate service
class PhysicalDisplayService {
    Surface* getExclusiveSurface() {
        // Return direct access to physical display
        return mPhysicalDisplay->createSurface();
    }
};
```

## Comparison with Existing Solutions

| Feature | VirtualDisplay API | Our Virtual Display HAL |
|---------|-------------------|------------------------|
| Default display | No | Yes (primary) |
| All apps use it | No (opt-in) | Yes (automatic) |
| Protected content | Limited | Full support |
| Latency | Higher | Minimal (~8ms) |
| HDR support | Limited | Full |
| Vsync aligned | No | Yes |

## Use Cases

### 1. Custom Compositor
```kotlin
// Apply system-wide color correction
val renderer = DisplayRenderer()
renderer.addEffect(ColorBlindnessFilter(type = DEUTERANOPIA))
renderer.start()
```

### 2. AR Pass-Through
```kotlin
// Overlay Android UI on camera feed
val cameraFeed = cameraService.openCamera(...)
val renderer = DisplayRenderer()
renderer.setBackground(cameraFeed)
renderer.setBlendMode(BlendMode.OVERLAY)
renderer.start()
```

### 3. Display Recording
```kotlin
// Record everything with custom encoding
val renderer = DisplayRenderer()
renderer.addOutput(H265Encoder(output = "recording.mp4"))
renderer.addOutput(PhysicalDisplay())
renderer.start()
```

### 4. Multi-Display
```kotlin
// Mirror to multiple outputs
val renderer = DisplayRenderer()
renderer.addOutput(PhysicalDisplay(id = 0))
renderer.addOutput(PhysicalDisplay(id = 1))  // HDMI
renderer.addOutput(NetworkStream(url = "rtsp://..."))
renderer.start()
```

## Implementation Phases

### Phase 1: Minimal HWC HAL
- [ ] Implement IComposer/IComposerClient
- [ ] Register virtual display as primary
- [ ] Output frames to BufferQueue
- [ ] Basic renderer app (passthrough)

### Phase 2: Physical Display Service
- [ ] Create physical display access service
- [ ] Vulkan surface for renderer
- [ ] Vsync coordination

### Phase 3: Effects Pipeline
- [ ] Shader-based effects
- [ ] Multi-output support
- [ ] HDR tone mapping

### Phase 4: Dynamic Configuration
- [ ] Resolution/refresh rate changes
- [ ] Hot-plug simulation
- [ ] Power state management

## File Structure

```
virtual-display/
├── hal/
│   ├── Android.bp
│   ├── VirtualDisplayComposer.cpp
│   ├── VirtualDisplayComposer.h
│   ├── VirtualDisplayClient.cpp
│   ├── VirtualDisplayClient.h
│   ├── VirtualDisplay.cpp
│   ├── VirtualDisplay.h
│   └── service.cpp
├── service/
│   ├── IPhysicalDisplayService.aidl
│   ├── IVirtualDisplayControl.aidl
│   └── PhysicalDisplayService.java
├── renderer-app/
│   ├── app/src/main/cpp/
│   │   ├── vulkan_renderer.cpp
│   │   └── effects/
│   └── app/src/main/java/
├── sepolicy/
│   └── ...
└── config/
    └── display_config.xml
```

## Design Decisions

1. **App-controlled registration** — Any installed app can register as display renderer and specify resolution/characteristics. System handles the transition (apps receive onConfigurationChanged).

2. **Fallback** — If renderer app crashes or unregisters, system transitions back to physical display (or passthrough mode).

3. **Runtime transitions are OK** — Brief flicker during resolution change is acceptable. Same mechanism Android uses for foldables, HDMI, etc.

4. **No special permissions for basic use** — Any app can register (like virtual camera/mic). System permissions for protected content.

## App-Controlled Display API

```kotlin
// Any app can become the display renderer
val displayService = VirtualDisplayManager.getInstance(context)

// Define virtual display characteristics
val config = VirtualDisplayConfig.Builder()
    .setWidth(3840)           // 4K
    .setHeight(2160)
    .setRefreshRate(120f)     // High refresh
    .setDensityDpi(320)
    .setHdrCapabilities(HDR10, HLG, DOLBY_VISION)
    .setColorMode(DISPLAY_P3)
    .setName("AR Compositor")
    .build()

// Take over display! System triggers configuration change.
val session = displayService.registerDisplay(config)

// Receive composited frames from all apps
session.setFrameCallback { frame: HardwareBuffer,
                           
                           
                            
                            
                             
                              
                               
                                
                                 
                                  
                                   
                                    
                                     
                                      
                                       
                                        
                                         
                                          
                                           
                                            
                                             
                                              
                                               
                                                
                                                 
                                                  
                                                   
                                                    
                                                     
                                                      timestamp: Long,
                           
                           
                            
                            
                             
                              
                               
                                
                                 
                                  
                                   
                                    
                                     
                                      
                                       
                                        
                                         
                                          
                                           
                                            
                                             
                                              
                                               
                                                
                                                 
                                                  
                                                   
                                                    
                                                     
                                                       
                                                        
                                                         
                                                          
                                                           
                                                            
                                                             
                                                              
                                                               
                                                                
                                                                 
                                                                  
                                                                   
                                                                    
                                                                     
                                                                       
                                                                        
                                                                         
                                                                           
                                                                            
                                                                             
                                                                              
                                                                               
                                                                                 
                                                                                  
                                                                                   
                                                                                     
                                                                                       
                                                                                        
                                                                                          
                                                                                           
                                                                                            
                                                                                              
                                                                                               
                                                                                                
                                                                                                 
                                                                                                   
                                                                                                     
                                                                                                      
                                                                                                       
                                                                                                         
                                                                                                          
                                                                                                            
                                                                                                              
                                                                                                               fence: SyncFence ->
    // Apply custom effects
    val processed = applyEffects(frame)
    
    // Render to physical display via Vulkan
    vulkanRenderer.render(processed)
    
    // Signal completion
    fence.signal()
}

// When done, release display
// System transitions back to physical (with transition)
session.unregister()
```

## Transition Flow

```
┌─────────────────────────────────────────────────────────────────┐
│ 1. App calls registerDisplay(4K config)                        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ 2. VirtualDisplayHAL creates virtual display                   │
│    - Notifies SurfaceFlinger: "new primary display config"     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ 3. SurfaceFlinger → WindowManager → Apps                       │
│    - All apps receive onConfigurationChanged()                 │
│    - Activities may restart or resize                          │
│    - ~500ms transition                                          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│ 4. System now running at 4K                                    │
│    - All apps render to 4K virtual display                     │
│    - Renderer app receives 4K frames                           │
│    - Renderer outputs to physical display                      │
└─────────────────────────────────────────────────────────────────┘
```

## Open Questions

1. **Protected content** — DRM/HDCP handling?
2. **Input routing** — Touch events map to virtual display coords?
3. **Overlay UI** — System UI (status bar, nav) rendered where?
4. **Multiple renderers** — Queue? Priority? First-come-first-served?

## References

- HWC AIDL HAL: `hardware/interfaces/graphics/composer/aidl/`
- SurfaceFlinger: `frameworks/native/services/surfaceflinger/`
- VirtualDisplay: `frameworks/base/core/java/android/hardware/display/`
- Composer HAL impl: `hardware/interfaces/graphics/composer/*/default/`
