# Golden Cube Virtual Camera Renderer

A sample app that renders a rotating golden 3D cube via Vulkan and provides it as a virtual camera source.

## What it does

1. Registers as a virtual camera renderer with `VirtualCameraService`
2. When a camera client (Zoom, Meet, etc.) opens the virtual camera:
   - Receives a Surface to render to
   - Starts Vulkan render loop
   - Renders a rotating golden cube at up to 4K60

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Golden Cube App                          │
│  ┌────────────────────────────────────────────────────────┐│
│  │  MainActivity.kt                                       ││
│  │  - Binds to VirtualCameraService                       ││
│  │  - Registers camera (4K60 RGBA)                        ││
│  │  - Receives surfaces via callback                      ││
│  └────────────────────┬───────────────────────────────────┘│
│                       │ JNI                                 │
│  ┌────────────────────▼───────────────────────────────────┐│
│  │  VulkanRenderer (C++)                                  ││
│  │  - Creates Vulkan instance/device                      ││
│  │  - Creates swapchain from ANativeWindow                ││
│  │  - Renders cube with Phong shading                     ││
│  │  - Presents at 60fps                                   ││
│  └────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
```

## Files

```
app/src/main/
├── java/com/example/vcamrenderer/
│   └── MainActivity.kt         # Main activity, service binding
├── cpp/
│   ├── CMakeLists.txt         # Build config
│   ├── vcam_renderer.cpp      # JNI interface
│   ├── vulkan_renderer.h/cpp  # Vulkan renderer
│   ├── cube_geometry.h/cpp    # Cube mesh + pipeline setup
│   └── shaders.h              # Embedded SPIR-V shaders
└── res/
    ├── layout/activity_main.xml
    └── drawable/ic_cube.xml   # Golden cube icon
```

## The Golden Cube

- **24 vertices** (4 per face, for proper normals)
- **36 indices** (2 triangles per face)
- **Phong shading** with:
  - Ambient: 0.2 × gold
  - Diffuse: light-angle × gold  
  - Specular: shininess 64, white-gold highlight
- **Auto-rotation** around Y axis

## Shaders

**Vertex shader:**
```glsl
// Transform vertices, pass color/normal to fragment
gl_Position = proj * view * model * vec4(position, 1.0);
fragNormal = mat3(transpose(inverse(model))) * normal;
```

**Fragment shader:**
```glsl
// Phong lighting with golden color
vec3 ambient = 0.2 * fragColor;
vec3 diffuse = max(dot(norm, lightDir), 0.0) * fragColor;
vec3 specular = 0.8 * pow(max(dot(viewDir, reflect(-lightDir, norm)), 0.0), 64.0) * vec3(1.0, 0.9, 0.5);
outColor = vec4(ambient + diffuse + specular, 1.0);
```

## Building

Requires:
- Android NDK with Vulkan headers
- GLM (header-only math library)

```bash
# Add GLM to your NDK or include it in the project
# Then build with Gradle:
./gradlew assembleDebug
```

## Note

This app requires the `VirtualCameraService` to be running in the system. It won't work on stock Android — only on a custom AOSP build with the virtual camera service installed.

## Usage

1. Install on device with virtual camera service
2. Tap "Start" to register as camera
3. Open a camera app (Zoom, Google Meet, Camera)
4. Select "Golden Cube Camera" from camera list
5. See the rotating golden cube!
