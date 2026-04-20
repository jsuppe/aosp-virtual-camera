---
description: Run adb shell command on melchior-connected device (cuttlefish default)
allowed-tools: Bash
---

Run an adb shell command on the AOSP build server (melchior) via SSH.

## Device selection

- Cuttlefish emulator: `0.0.0.0:6520`
- Physical device: check with `ssh melchior "adb devices"`
- Default to cuttlefish unless user specifies otherwise

## How to execute

Run via Bash tool:

```bash
ssh melchior "adb -s 0.0.0.0:6520 shell '$ARGUMENTS'"
```

If the user provides no arguments, list devices instead:

```bash
ssh melchior "adb devices"
```

## Common patterns

- Get property: `ssh melchior "adb -s 0.0.0.0:6520 shell getprop ro.build.version.sdk"`
- Service list: `ssh melchior "adb -s 0.0.0.0:6520 shell dumpsys -l | grep camera"`
- SELinux mode: `ssh melchior "adb -s 0.0.0.0:6520 shell getenforce"`
- Install APK: `ssh melchior "adb -s 0.0.0.0:6520 install /path/to/app.apk"`
- Reboot: confirm with user first, then `ssh melchior "adb -s 0.0.0.0:6520 reboot"`

## Notes

- Multiple devices may be connected; always use `-s` flag
- Long-running commands: add timeout via `timeout 30 adb shell ...`
- Binary output (screencap etc): redirect to file on server, then scp
