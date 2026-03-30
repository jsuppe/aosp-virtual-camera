# Virtual Camera APEX

Vendor APEX packaging for the Virtual Camera HAL.

## What's Included

```
apex/
├── Android.bp                    # APEX build rules
├── apex_manifest.json            # APEX metadata
├── file_contexts                 # SELinux contexts
├── com.android.hardware.camera.provider.virtual.rc      # init script
├── com.android.hardware.camera.provider.virtual.pem     # Private key
├── com.android.hardware.camera.provider.virtual.pk8     # PKCS#8 key
├── com.android.hardware.camera.provider.virtual.x509.pem # Certificate
├── com.android.hardware.camera.provider.virtual.avbpubkey # AVB public key
└── generate_keys.sh              # Key generation script
```

## Building

### First Time Setup

1. Copy the `apex/` directory to your AOSP tree:
   ```bash
   cp -r apex/ $AOSP/hardware/interfaces/camera/provider/virtual/apex/
   ```

2. Copy the HAL files if not already present:
   ```bash
   cp -r hal/* $AOSP/hardware/interfaces/camera/provider/virtual/
   ```

3. Build the APEX:
   ```bash
   cd $AOSP
   source build/envsetup.sh
   lunch <your_target>
   m com.android.hardware.camera.provider.virtual
   ```

### Output

The built APEX will be at:
```
out/target/product/<device>/vendor/apex/com.android.hardware.camera.provider.virtual.apex
```

## Installation

### On a device with the APEX already in the build:

```bash
# Install updated APEX
adb install com.android.hardware.camera.provider.virtual.apex

# Or push and reboot
adb push com.android.hardware.camera.provider.virtual.apex /data/apex/
adb reboot
```

### First time (requires APEX in build):

The APEX must be included in the initial vendor image build. After that,
updates can be pushed via `adb install`.

## Signing Keys

The included keys are **for development only**. For production:

1. Generate new keys:
   ```bash
   ./generate_keys.sh
   ```

2. Store private keys securely
3. Use proper key management (HSM, etc.)

## Contents at Runtime

When installed, the APEX mounts at:
```
/apex/com.android.hardware.camera.provider.virtual/
├── bin/hw/android.hardware.camera.provider-virtual-service
├── lib64/hw/android.hardware.camera.provider-V1-virtual-impl.so
└── etc/
    ├── init/...
    └── vintf/...
```

## Updating

Since `supportsRebootlessUpdate: true`, the HAL can be updated without a full reboot:

```bash
adb install --staged com.android.hardware.camera.provider.virtual.apex
# HAL restarts automatically
```

## Troubleshooting

### APEX not activating
```bash
# Check APEX status
adb shell pm list packages | grep apex
adb shell dumpsys apexd

# Check logs
adb logcat | grep -i apex
```

### Service not starting
```bash
# Check if service is registered
adb shell service list | grep camera

# Check init logs
adb logcat -s init
```
