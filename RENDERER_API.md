# Virtual Camera & Mic — Renderer-Defined Characteristics

Both virtual camera and virtual mic follow the same model: **the renderer app defines how the virtual device appears to consumer apps**.

## Unified Model

```
┌─────────────────────────────────────────────────────────────────────┐
│                         RENDERER APP                                │
│  - Defines device characteristics (type, name, capabilities)       │
│  - Provides frames/samples                                          │
│  - Controls lifecycle (register → provide data → unregister)       │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      VIRTUAL DEVICE SERVICE                         │
│  - Manages renderer registration                                    │
│  - Dynamically creates/removes HAL devices                         │
│  - Routes data from renderer to HAL                                │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         VIRTUAL HAL                                 │
│  - Appears as real camera/mic to framework                         │
│  - Exposes renderer-defined characteristics                        │
│  - Provides frames/samples to consumer apps                        │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│                        CONSUMER APP                                 │
│  - Uses standard Android APIs (Camera2, AudioRecord)               │
│  - Sees virtual device in device list                              │
│  - No code changes needed                                           │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Virtual Camera — Renderer API

### Configuration

```kotlin
val cameraConfig = VirtualCameraConfig.Builder()
    // Device identity
    .setCameraId("virtual_camera_001")
    .setDisplayName("AR Effects Camera")
    
    // Camera characteristics
    .setLensFacing(CameraCharacteristics.LENS_FACING_FRONT)  // or BACK, EXTERNAL
    .setSensorOrientation(0)  // 0, 90, 180, 270
    
    // Supported stream configurations
    .addStreamConfig(
        format = ImageFormat.YUV_420_888,
        width = 1920, height = 1080,
        minFps = 30, maxFps = 60
    )
    .addStreamConfig(
        format = ImageFormat.YUV_420_888,
        width = 1280, height = 720,
        minFps = 30, maxFps = 60
    )
    .addStreamConfig(
        format = ImageFormat.JPEG,  // For still capture
        width = 1920, height = 1080,
        minFps = 1, maxFps = 30
    )
    
    // Hardware level
    .setHardwareLevel(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL)
    
    // Optional: Additional metadata
    .setFlashAvailable(false)
    .setAutoFocusModes(listOf(CONTROL_AF_MODE_OFF))
    .build()
```

### Session

```kotlin
val virtualCamera = VirtualCameraManager.getInstance(context)

// Register camera (appears in CameraManager.getCameraIdList())
val session = virtualCamera.registerCamera(cameraConfig)

// When consumer app opens camera and requests stream:
session.setOnStreamRequestListener { streamConfig ->
    // Consumer wants: streamConfig.width x streamConfig.height @ streamConfig.format
    // Start providing frames at this resolution
}

// Provide frames
val surface = session.getInputSurface()  // Render to this surface
// OR
session.submitFrame(imageBuffer, timestamp)  // Submit raw buffer

// When done
session.unregister()  // Camera disappears from device list
```

### AIDL Interface

```aidl
// IVirtualCameraService.aidl
interface IVirtualCameraService {
    // Register a new virtual camera with specified characteristics
    IVirtualCameraSession registerCamera(in VirtualCameraConfig config);
    
    // List currently registered virtual cameras
    List<VirtualCameraInfo> getRegisteredCameras();
}

// VirtualCameraConfig.aidl
parcelable VirtualCameraConfig {
    String cameraId;          // Unique ID for this camera
    String displayName;       // Human-readable name
    
    int lensFacing;           // FRONT, BACK, EXTERNAL
    int sensorOrientation;    // 0, 90, 180, 270
    int hardwareLevel;        // EXTERNAL, LIMITED, FULL, LEVEL_3
    
    StreamConfig[] streamConfigs;  // Supported resolutions/formats/fps
    
    // Optional capabilities
    boolean flashAvailable;
    int[] autoFocusModes;
    int[] autoExposureModes;
}

// StreamConfig.aidl
parcelable StreamConfig {
    int format;      // YUV_420_888, JPEG, RGBA_8888, etc.
    int width;
    int height;
    int minFps;
    int maxFps;
}

// IVirtualCameraSession.aidl
interface IVirtualCameraSession {
    // Get surface to render frames to
    Surface getInputSurface(int width, int height, int format);
    
    // Or submit raw frame buffer
    void submitFrame(in HardwareBuffer buffer, long timestampNs);
    
    // Called when consumer configures streams
    void setStreamRequestCallback(IStreamRequestCallback callback);
    
    // Unregister camera
    void unregister();
}
```

---

## Virtual Microphone — Renderer API

### Configuration

```kotlin
val micConfig = VirtualMicConfig.Builder()
    // Device identity
    .setDeviceType(AudioDeviceInfo.TYPE_USB_DEVICE)
    .setProductName("AI Voice Generator")
    .setAddress("virtual_mic_ai_001")
    
    // Advertised capabilities
    .setSupportedSampleRates(48000, 44100, 16000)
    .setSupportedChannelMasks(CHANNEL_IN_MONO, CHANNEL_IN_STEREO)
    .setSupportedEncodings(ENCODING_PCM_16BIT, ENCODING_PCM_FLOAT)
    
    // Session format
    .setPreferredSampleRate(48000)
    .setPreferredChannelMask(CHANNEL_IN_STEREO)
    .setPreferredEncoding(ENCODING_PCM_16BIT)
    .setBufferSizeMs(10)
    .build()
```

### Session

```kotlin
val virtualMic = VirtualMicManager.getInstance(context)

// Register mic (appears in AudioManager.getDevices())
val session = virtualMic.registerMic(micConfig)

// Provide audio samples
val buffer = ShortArray(1024)
while (rendering) {
    generateAudio(buffer)
    session.write(buffer, 0, buffer.size)
}

// When done
session.unregister()  // Mic disappears from device list
```

---

## Comparison

| Aspect | Virtual Camera | Virtual Mic |
|--------|---------------|-------------|
| Device identity | cameraId, displayName | deviceType, productName, address |
| Facing/Type | FRONT, BACK, EXTERNAL | BUILTIN_MIC, USB_DEVICE, etc. |
| Capabilities | Resolutions, formats, FPS | Sample rates, channels, encoding |
| Data format | YUV/RGBA frames | PCM samples |
| Data delivery | Surface or HardwareBuffer | write() to buffer |
| Latency target | ~16-33ms (30-60fps) | <10ms |

---

## Use Case Examples

### Virtual Camera: AR Filter App
```kotlin
// Register as front camera with processed video
val config = VirtualCameraConfig.Builder()
    .setLensFacing(LENS_FACING_FRONT)
    .setDisplayName("AR Beauty Camera")
    .addStreamConfig(YUV_420_888, 1080, 1920, 30, 30)
    .build()

// Real front camera → AR processing → virtual camera → video call app
```

### Virtual Camera: Screen Capture as Camera
```kotlin
// Register as external camera showing screen content
val config = VirtualCameraConfig.Builder()
    .setLensFacing(LENS_FACING_EXTERNAL)
    .setDisplayName("Screen Share")
    .addStreamConfig(YUV_420_888, 1920, 1080, 30, 60)
    .build()

// Screen capture → virtual camera → video call app
```

### Virtual Mic: AI Voice Clone
```kotlin
// Register as USB mic with AI-generated voice
val config = VirtualMicConfig.Builder()
    .setDeviceType(TYPE_USB_DEVICE)
    .setProductName("AI Voice Clone")
    .setPreferredSampleRate(48000)
    .build()

// Text → TTS → virtual mic → voice chat app
```

### Virtual Mic: Game Audio to Chat
```kotlin
// Register as headset mic with game audio
val config = VirtualMicConfig.Builder()
    .setDeviceType(TYPE_WIRED_HEADSET)
    .setProductName("Game Audio Share")
    .setPreferredSampleRate(48000)
    .build()

// Game audio → virtual mic → Discord/chat app
```

---

## Dynamic Device Appearance

Key benefit: **devices appear/disappear dynamically** based on renderer registration.

```
Time 0:00 - Device list: [Built-in Mic, Built-in Camera]
Time 0:01 - AR app registers virtual camera
Time 0:01 - Device list: [Built-in Mic, Built-in Camera, "AR Beauty Camera"]
Time 0:05 - User opens video call, selects "AR Beauty Camera"
Time 0:30 - AR app unregisters
Time 0:30 - Device list: [Built-in Mic, Built-in Camera]
```

Consumer apps see devices come and go just like plugging/unplugging USB devices!
