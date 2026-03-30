/*
 * VirtualMicConfig - Configuration for virtual microphone registration
 */
package com.android.virtualmicservice;

parcelable VirtualMicConfig {
    // --- Device Identity (how it appears to consumer apps) ---
    
    /**
     * Device type from AudioDeviceInfo.TYPE_*
     * e.g., TYPE_BUILTIN_MIC (15), TYPE_USB_DEVICE (11), TYPE_WIRED_HEADSET (3)
     */
    int deviceType = 15;  // Default: BUILTIN_MIC
    
    /**
     * Human-readable product name shown in device list.
     * e.g., "AI Voice Generator", "Game Audio Share"
     */
    String productName = "Virtual Microphone";
    
    /**
     * Unique device address/identifier.
     */
    String address = "virtual_mic_default";
    
    // --- Advertised Capabilities (what consumer apps see) ---
    
    /**
     * Supported sample rates in Hz.
     */
    int[] supportedSampleRates = {48000, 44100, 16000};
    
    /**
     * Supported channel masks (AudioFormat.CHANNEL_IN_*).
     */
    int[] supportedChannelMasks;
    
    /**
     * Supported audio encodings (AudioFormat.ENCODING_*).
     */
    int[] supportedEncodings;
    
    // --- Session Parameters (what renderer will provide) ---
    
    /**
     * Sample rate the renderer will provide.
     */
    int sampleRate = 48000;
    
    /**
     * Channel mask the renderer will provide.
     * e.g., CHANNEL_IN_MONO (16), CHANNEL_IN_STEREO (12)
     */
    int channelMask = 12;  // STEREO
    
    /**
     * Audio encoding the renderer will provide.
     * e.g., ENCODING_PCM_16BIT (2), ENCODING_PCM_FLOAT (4)
     */
    int encoding = 2;  // PCM_16BIT
    
    /**
     * Requested buffer size in milliseconds.
     * Lower = less latency but more underruns.
     */
    int bufferSizeMs = 20;
}
