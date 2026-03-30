# Virtual Microphone HAL

A custom Android Audio HAL that allows apps to act as "audio renderers" — feeding audio that appears as microphone input to other apps.

## Architecture

```
┌─────────────────┐     ┌──────────────────────┐     ┌─────────────────┐
│  Renderer App   │────▶│  VirtualMicService   │────▶│  VirtualMicHAL  │
│  (audio source) │     │  (system service)    │     │  (mic device)   │
└─────────────────┘     └──────────────────────┘     └────────┬────────┘
                                                              │
                                                              ▼
                                                     ┌─────────────────┐
                                                     │  Consumer App   │
                                                     │  (records mic)  │
                                                     └─────────────────┘
```

## Components

### 1. VirtualMicHAL (`/vendor/lib64/hw/`)
- Implements `IModule` / `IStreamIn` (AIDL Audio HAL)
- Registers as an audio input device (microphone)
- Receives PCM frames from VirtualMicService
- Provides frames to apps requesting mic input

### 2. VirtualMicService (System Service)
- AIDL interface for renderer apps to connect
- Manages audio routing between renderers and HAL
- Handles format conversion if needed
- Permission checks for renderer apps

### 3. Renderer SDK (Client Library)
- Java/Kotlin API for apps to provide audio
- Handles connection to VirtualMicService
- Audio format negotiation

## Audio HAL Interface

Modern Android uses AIDL Audio HAL:
- `hardware/interfaces/audio/aidl/`
- Key interfaces: `IModule`, `IStreamIn`, `IStreamOut`

### Stream Configuration
```
Format: PCM 16-bit
Sample Rate: 48000 Hz (primary), 44100 Hz, 16000 Hz
Channels: Mono, Stereo
Buffer: ~10-20ms per period (low latency)
```

## Key Differences from Camera HAL

| Aspect | Camera HAL | Audio HAL |
|--------|------------|-----------|
| Data type | Discrete frames | Continuous stream |
| Latency requirement | ~33ms (30fps) | ~10-20ms |
| Buffer mechanism | GraphicBuffer / Surface | FMQ (Fast Message Queue) |
| Format | YUV/RGBA pixels | PCM samples |
| Framework interface | ICameraProvider | IModule |

## Implementation Phases

### Phase 1: Minimal HAL
- [ ] Implement IModule with single virtual mic device
- [ ] Implement IStreamIn for audio capture
- [ ] Generate test tone (sine wave) for validation
- [ ] Register in audio_policy_configuration.xml

### Phase 2: Service Integration
- [ ] Create VirtualMicService AIDL interface
- [ ] System service registration
- [ ] HAL ↔ Service communication (shared memory or FMQ)

### Phase 3: Renderer API
- [ ] Client SDK for renderer apps
- [ ] Format negotiation
- [ ] Permission model

## File Structure

```
virtual-mic/
├── hal/
│   ├── Android.bp
│   ├── VirtualMicModule.cpp      # IModule implementation
│   ├── VirtualMicModule.h
│   ├── VirtualMicStreamIn.cpp    # IStreamIn implementation
│   ├── VirtualMicStreamIn.h
│   └── service.cpp               # HAL service entry
├── service/
│   ├── IVirtualMicService.aidl
│   ├── IVirtualMicRenderer.aidl
│   └── VirtualMicService.java
├── sepolicy/
│   ├── file_contexts
│   ├── service_contexts
│   └── hal_audio_virtual.te
└── config/
    └── virtual_mic_audio_policy_configuration.xml
```

## Audio Policy Configuration

```xml
<module name="virtual_mic" halVersion="3.0">
    <attachedDevices>
        <item>Virtual Mic In</item>
    </attachedDevices>
    <mixPorts>
        <mixPort name="virtual_mic input" role="sink">
            <profile name="" format="AUDIO_FORMAT_PCM_16_BIT"
                     samplingRates="48000 44100 16000" 
                     channelMasks="AUDIO_CHANNEL_IN_MONO AUDIO_CHANNEL_IN_STEREO"/>
        </mixPort>
    </mixPorts>
    <devicePorts>
        <devicePort tagName="Virtual Mic In" 
                    type="AUDIO_DEVICE_IN_BUILTIN_MIC" 
                    role="source">
            <profile name="" format="AUDIO_FORMAT_PCM_16_BIT"
                     samplingRates="48000" 
                     channelMasks="AUDIO_CHANNEL_IN_STEREO"/>
        </devicePort>
    </devicePorts>
    <routes>
        <route type="mix" sink="virtual_mic input"
               sources="Virtual Mic In"/>
    </routes>
</module>
```

## Use Cases

1. **AI Voice Generation** — TTS/voice clone output appears as mic
2. **Audio Routing** — App A's output → App B's mic input
3. **Testing** — Inject test audio for voice apps
4. **Accessibility** — Alternative input methods
5. **ClipKid** — AI-generated narration recordable by any app

## Comparison with r_submix

| Feature | r_submix (built-in) | Virtual Mic HAL |
|---------|---------------------|-----------------|
| Access | System/MediaProjection | Any permitted app |
| Direction | System audio → capture | App audio → mic |
| Latency | ~85ms | Target: ~20ms |
| Format | Fixed 48kHz stereo | Negotiable |
| Use case | Screen recording | Audio injection |

## Design Decisions

1. **Permission model** — Open to any installed app. No special permissions required to inject audio.

2. **Device selection** — Works like camera selection:
   ```kotlin
   // Consumer app lists available mics
   val mics = audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS)
   
   // Find and select virtual mic
   val virtualMic = mics.find { it.productName == "Virtual Mic" }
   audioRecord.setPreferredDevice(virtualMic)
   ```
   Virtual mic appears as `TYPE_BUILTIN_MIC` with descriptive name.

3. **Multiple renderers** — Exclusive for now. One renderer at a time.

4. **Latency target** — Real-time, as low as possible. Target: <10ms.

5. **Echo cancellation** — Not needed. Up to the consumer app to handle.

## Renderer API Design

Simple API for apps to inject audio:

```kotlin
// Get the virtual mic service
val virtualMic = VirtualMicManager.getInstance(context)

// Define how this virtual mic appears to consumer apps
val config = VirtualMicConfig.Builder()
    // Device identity (how it appears in device list)
    .setDeviceType(AudioDeviceInfo.TYPE_USB_DEVICE)  // or BUILTIN_MIC, etc.
    .setProductName("AI Voice Assistant")
    .setAddress("virtual_mic_ai_voice")
    
    // Advertised capabilities (what consumer apps see)
    .setSupportedSampleRates(48000, 44100, 16000)
    .setSupportedChannelMasks(CHANNEL_IN_MONO, CHANNEL_IN_STEREO)
    .setSupportedEncodings(ENCODING_PCM_16BIT, ENCODING_PCM_FLOAT)
    
    // Actual session format (what renderer provides)
    .setPreferredSampleRate(48000)
    .setPreferredChannelMask(CHANNEL_IN_STEREO)
    .setPreferredEncoding(ENCODING_PCM_16BIT)
    .setBufferSizeMs(10)  // Low latency
    .build()

// Register and start session (exclusive access)
val session = virtualMic.startSession(config)

// Write audio frames
val buffer = ShortArray(1024)
while (rendering) {
    generateAudio(buffer)
    session.write(buffer, 0, buffer.size)
}

// Release when done (mic disappears from device list)
session.close()
```

### Service AIDL Interface

```aidl
// IVirtualMicService.aidl
interface IVirtualMicService {
    // Check if virtual mic is available
    boolean isAvailable();
    
    // Start exclusive rendering session
    // Returns null if another renderer is active
    IVirtualMicSession startSession(in VirtualMicConfig config);
    
    // Get current renderer info (or null if none)
    RendererInfo getCurrentRenderer();
}

// IVirtualMicSession.aidl
interface IVirtualMicSession {
    // Write PCM samples to virtual mic
    // Returns number of samples written
    int write(in short[] buffer, int offset, int size);
    
    // Get current buffer state
    int getBufferSizeInFrames();
    int getUnderrunCount();
    
    // Release the session
    void close();
}

// VirtualMicConfig.aidl
parcelable VirtualMicConfig {
    // Device characteristics (how it appears to consumer apps)
    int deviceType;           // BUILTIN_MIC, USB_DEVICE, WIRED_HEADSET, etc.
    String productName;       // "AI Voice", "Game Audio", etc.
    String address;           // Unique identifier
    
    // Audio format capabilities
    int[] sampleRates;        // [48000, 44100, 16000]
    int[] channelMasks;       // [MONO, STEREO]
    int[] encodings;          // [PCM_16BIT, PCM_FLOAT]
    
    // Session parameters
    int preferredSampleRate;  // What renderer will actually provide
    int preferredChannelMask;
    int preferredEncoding;
    int bufferSizeMs;         // Suggested buffer size
}
```

## Consumer App Usage

Apps recording from virtual mic (no changes needed — standard Android API):

```kotlin
// List mics and find virtual one
val audioManager = getSystemService(AudioManager::class.java)
val virtualMic = audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS)
    .find { it.productName == "Virtual Mic" }

// Create AudioRecord targeting virtual mic
val audioRecord = AudioRecord.Builder()
    .setAudioSource(MediaRecorder.AudioSource.MIC)
    .setAudioFormat(AudioFormat.Builder()
        .setSampleRate(48000)
        .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
        .build())
    .build()

// Route to virtual mic
audioRecord.setPreferredDevice(virtualMic)

// Record normally
audioRecord.startRecording()
val buffer = ShortArray(1024)
while (recording) {
    val read = audioRecord.read(buffer, 0, buffer.size)
    // Process audio...
}
```

## References

- AOSP Audio HAL: `hardware/interfaces/audio/aidl/`
- r_submix: `hardware/libhardware/modules/audio_remote_submix/`
- Audio Policy: `frameworks/av/services/audiopolicy/`
- AudioDeviceInfo: `frameworks/base/media/java/android/media/AudioDeviceInfo.java`
- AudioRecord: `frameworks/base/media/java/android/media/AudioRecord.java`
