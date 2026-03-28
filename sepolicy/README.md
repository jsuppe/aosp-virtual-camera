# Virtual Camera SELinux Policies

## Overview

For virtual camera to work with SELinux enforcing, three domains need permissions:

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│ Renderer App    │     │ system_server   │     │ HAL             │
│ (untrusted_app) │     │                 │     │ (hal_camera_    │
│                 │     │                 │     │      virtual)   │
├─────────────────┤     ├─────────────────┤     ├─────────────────┤
│ • Use service   │     │ • Host services │     │ • Connect to    │
│ • Receive FDs   │────▶│ • Create ashmem │────▶│   system_server │
│ • Map shmem     │     │ • Alloc buffers │     │ • Use shmem     │
│ • GPU render    │     │ • Pass FDs      │     │ • Use buffers   │
│ • Create fences │     │                 │     │ • Camera HAL    │
└─────────────────┘     └─────────────────┘     └─────────────────┘
```

## Files

| File | Description | Target Location |
|------|-------------|-----------------|
| `hal_camera_virtual.te` | HAL domain policy | `device/.../sepolicy/vendor/` |
| `system_server.te` | system_server additions | `system/sepolicy/private/` |
| `untrusted_app.te` | App permissions | `system/sepolicy/private/` |
| `service.te` | Service type definitions | `system/sepolicy/private/` |
| `service_contexts` | Service labels | `system/sepolicy/private/` |
| `hwservice_contexts` | HAL service labels | `device/.../sepolicy/vendor/` |
| `file_contexts` | Binary labels | `device/.../sepolicy/vendor/` |

## Key Permissions Explained

### Shared Memory (ashmem)

```te
# Create ashmem region
allow <domain> ashmem_device:chr_file rw_file_perms;

# Map it into address space  
allow <domain> ashmem_libcutils_device:chr_file { read write map };
```

### HardwareBuffer / DMA-BUF

```te
# Allocate via gralloc
hal_client_domain(<domain>, hal_graphics_allocator)

# Access underlying DMA-BUF
allow <domain> dmabuf_system_heap_device:chr_file r_file_perms;
```

### Binder FD Passing

```te
# Sender can transfer FD
allow system_server hal_camera_virtual:fd use;

# Receiver can use FD from sender
allow hal_camera_virtual system_server:fd use;
```

### GPU Sync Fences

```te
# Create/wait on sync fences
allow <domain> sync_device:chr_file { read write ioctl };
```

## Installation

### 1. Add to device makefile

```makefile
# device/<vendor>/<device>/device.mk

BOARD_VENDOR_SEPOLICY_DIRS += device/<vendor>/<device>/sepolicy/vendor
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/<vendor>/<device>/sepolicy/private
```

### 2. Create directory structure

```
device/<vendor>/<device>/sepolicy/
├── vendor/
│   ├── hal_camera_virtual.te
│   ├── hwservice_contexts
│   └── file_contexts
└── private/
    ├── system_server.te
    ├── untrusted_app.te
    ├── service.te
    └── service_contexts
```

### 3. Build and test

```bash
# Build
m selinux_policy

# Check for denials
adb shell dmesg | grep -i avc
adb logcat | grep -i selinux
```

## Debugging

### Check current context

```bash
# Process context
adb shell ps -Z | grep -E "camera|virtual"

# File context  
adb shell ls -Z /vendor/bin/hw/android.hardware.camera.provider-virtual-service

# Service context
adb shell service list | grep virtual_camera
```

### Temporarily permissive (for debugging only!)

```bash
# Make HAL domain permissive
adb shell setenforce 0  # DANGER: disables all SELinux

# Or add to policy (build-time):
permissive hal_camera_virtual;  # Only makes this domain permissive
```

### Generate policy from denials

```bash
# Capture denials
adb shell dmesg | grep avc > denials.txt

# Generate policy (on host)
audit2allow -i denials.txt -p out/target/product/<device>/vendor/etc/selinux/
```

## Security Considerations

### Renderer App Permissions

The default `untrusted_app.te` gives **all apps** virtual camera access. For production:

1. **Create dedicated domain** (`virtual_camera_renderer`)
2. **Require signature permission** in service
3. **Allowlist specific apps** by package name

```te
# More restrictive: dedicated renderer domain
type virtual_camera_renderer, domain;
app_domain(virtual_camera_renderer)

# Only this domain can use virtual camera
allow virtual_camera_renderer virtual_camera_service:service_manager find;
neverallow untrusted_app virtual_camera_service:service_manager find;
```

### HAL Isolation

The HAL should not have broad permissions:

```te
# Deny network access
neverallow hal_camera_virtual { domain -init }:tcp_socket *;
neverallow hal_camera_virtual { domain -init }:udp_socket *;

# Deny file system writes (except logging)
neverallow hal_camera_virtual { file_type -log_file }:file write;
```
