---
description: Build AOSP module on melchior via tmux
allowed-tools: Bash
---

Build an AOSP module on melchior using `m <module>` in a tmux session.

## How to execute

### Start build

The argument is the module name (e.g. `sepolicy`, `android.hardware.camera.provider-service`).

```bash
ssh melchior "ssh melchior@localhost 'tmux kill-session -t vcam_build 2>/dev/null; tmux new-session -d -s vcam_build \"cd /mnt/micron/aosp && source build/envsetup.sh && lunch aosp_cf_x86_64_only_phone-trunk_staging-userdebug && m $ARGUMENTS 2>&1 | tee /tmp/aosp_build.log; echo __BUILD_EXIT=\\\$?\"'"
```

Note: runs as `melchior` user (owns the AOSP tree).

### Check status

```bash
ssh melchior "ssh melchior@localhost 'tmux has-session -t vcam_build 2>/dev/null && echo RUNNING || echo DONE; tail -30 /tmp/aosp_build.log'"
```

### Check if build finished

Look for `__BUILD_EXIT=` marker in log:

```bash
ssh melchior "ssh melchior@localhost 'grep __BUILD_EXIT /tmp/aosp_build.log || echo still_running'"
```

## Common modules

- `sepolicy` — SELinux policy (after .te edits)
- `android.hardware.camera.provider-service` — camera HAL
- `VirtualCameraService` — vcam Java service
- `nothing` — just generate intermediates (compdb, AIDL stubs)

## Notes

- Builds run as `melchior` user via double SSH hop
- tmux session persists if SSH drops
- Full log at `/tmp/aosp_build.log`
- Lunch target: `aosp_cf_x86_64_only_phone-trunk_staging-userdebug`
