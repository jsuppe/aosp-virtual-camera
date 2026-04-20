---
description: Tail filtered logcat or dmesg AVC denials from device on melchior
allowed-tools: Bash
---

Fetch logcat or kernel dmesg from a device connected to melchior.

## How to execute

### Logcat (filtered)

If user provides a filter pattern (e.g. "camera", "vcam", "SELinux"):

```bash
ssh melchior "adb -s 0.0.0.0:6520 logcat -d -b main,system,crash | grep -iE '$ARGUMENTS' | tail -200"
```

If no pattern, dump recent:

```bash
ssh melchior "adb -s 0.0.0.0:6520 logcat -d -b main,system,crash | tail -100"
```

### AVC denials (SELinux)

If user asks about SELinux denials, AVC, or sepolicy:

```bash
ssh melchior "adb -s 0.0.0.0:6520 shell dmesg | grep -i 'avc:' | tail -100"
```

This output feeds directly into the sepolicy-aosp MCP `parse_denials` tool for analysis.

## Buffer options

- `main,system,crash` — default, most useful
- `all` — everything including radio, events (noisy)
- `events` — binder/intent lifecycle events

## Notes

- Use `-d` flag (dump and exit), not streaming
- Always pipe through `tail` to limit output
- For cleared-state capture: `ssh melchior "adb -s 0.0.0.0:6520 logcat -c"` then re-read
- Default device is cuttlefish `0.0.0.0:6520`
