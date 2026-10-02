# Architecture

## 1. Why not MediaProjection?

MediaProjection captures the final display output after the target app has rendered. That adds another producer/consumer path and makes frame timing harder.

This project instead aims to control the graphics boundary of a guest app.

## 2. Proposed architecture

```
Android host process
|
+-- UI / launcher
|
+-- Virtual Runtime
|   +-- package/service virtualization
|   +-- guest Activity lifecycle
|   +-- guest native library loading
|
+-- Graphics Interceptor
|   +-- Vulkan dispatch interception
|   +-- EGL/OpenGL ES interception (later)
|   +-- swapchain/surface tracking
|
+-- Frame Bridge
|   +-- GPU copy / image ownership transitions
|   +-- AHardwareBuffer or Vulkan external memory
|   +-- synchronization fences/semaphores
|
+-- FrameGen Backend
|   +-- pluggable interface
|   +-- real frame A
|   +-- real frame B
|   +-- generated intermediate frames
|
+-- Host Compositor
    +-- pacing
    +-- generated-frame presentation
    +-- scaling/sharpening
    +-- visible Surface
```

## 3. Virtual runtime

A VirtualApp-style engine is useful because it can execute a guest APK inside host-controlled stub processes and virtualize Android framework services.

It is not enough by itself. The important addition is controlling guest native graphics loading and presentation.

Responsibilities:

- install/import guest APKs into private host storage;
- launch guest Activities through host stub Activities;
- maintain virtual package/UID state;
- load guest native libraries inside a controlled process;
- initialize our native graphics interceptor before guest rendering begins.

## 4. Vulkan interception

The first target is Vulkan.

Functions of interest include:

- `vkGetInstanceProcAddr`
- `vkGetDeviceProcAddr`
- `vkCreateAndroidSurfaceKHR`
- `vkCreateSwapchainKHR`
- `vkAcquireNextImageKHR`
- `vkQueuePresentKHR`
- swapchain destruction/recreation

The interceptor should maintain its own dispatch table rather than assuming a system-wide implicit layer.

### First experiment

Do not begin with a commercial game.

Create a controlled Vulkan test guest and prove that the host can:

1. observe swapchain creation;
2. observe every present;
3. identify the presented image;
4. copy it into a host-owned Vulkan image;
5. render that image to another host-owned Surface.

If this cannot be made stable, the full virtual-app approach is not viable.

## 5. Frame bridge

Preferred path:

```
guest swapchain image
  -> Vulkan GPU copy
  -> shared/external image
  -> frame-generation compute
  -> compositor image
  -> host swapchain
```

Avoid CPU readback.

Potential Android primitive:

- `AHardwareBuffer` for shareable GPU-backed buffers;
- Vulkan external-memory Android hardware-buffer extensions;
- native fences/semaphores for synchronization.

The exact mechanism must be selected after device testing because Android GPU drivers differ.

## 6. Host compositor

The compositor owns the visible Surface and decides when real and generated frames are presented.

Long-term target:

```
30 FPS guest
real0 ---- real1 ---- real2
       gen0      gen1

60 Hz output
real0-gen0-real1-gen1-real2
```

The first compositor milestone should simply duplicate or re-present captured frames. This validates ownership and timing before adding an interpolation model.

## 7. Frame-generation backend API

The compositor must not depend on one specific algorithm.

Suggested native interface:

```cpp
struct FrameInput {
    VkImage image;
    uint32_t width;
    uint32_t height;
    uint64_t timestampNs;
};

struct FrameGenConfig {
    uint32_t multiplier;
    bool performanceMode;
};

class IFrameGenerator {
public:
    virtual ~IFrameGenerator() = default;
    virtual bool initialize(const FrameGenConfig&) = 0;
    virtual bool generate(
        const FrameInput& previous,
        const FrameInput& current,
        FrameInput* output,
        uint32_t outputCount) = 0;
};
```

This lets the project test different legal/open frame-generation implementations later.

## 8. OpenGL ES

After Vulkan works, add an EGL/OpenGL ES path.

Likely interception points include buffer swap and surface creation. Do not implement both APIs at the same time in the first milestone.

## 9. Full Android VM vs app virtualization

A full guest Android VM would give stronger control over graphics, but it is substantially heavier and hardware virtualization support is device-dependent.

For the initial Android-phone target, use lightweight app virtualization plus a host-owned compositor.

A gfxstream-like guest/host protocol is useful architectural inspiration if direct same-process interception becomes too fragile.

## 10. Milestones

### M0 - repository and native test harness
- Android ARM64 app
- Kotlin launcher
- C++/NDK module
- Vulkan device/surface test

### M1 - controlled guest
- import a test APK
- launch it inside virtual runtime
- initialize native interceptor before guest graphics

### M2 - direct frame access
- intercept Vulkan presentation
- GPU-copy real frames
- no MediaProjection

### M3 - compositor
- host-owned visible Surface
- stable re-presentation
- frame pacing metrics

### M4 - generated frames
- pluggable framegen backend
- 2x mode first
- latency and artifact metrics

### M5 - compatibility
- swapchain recreation
- rotation
- fullscreen
- OpenGL ES
- per-app profiles
