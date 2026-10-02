# M0.1 - Host-owned Vulkan Surface

M0.1 proves that the project can own the visible Android Surface and present directly to it through Vulkan.

## Data path

```
SurfaceView
  -> Java Surface
  -> ANativeWindow
  -> VkAndroidSurfaceKHR
  -> VkSwapchainKHR
  -> VkImage
  -> vkQueuePresentKHR
```

There is no MediaProjection in this path.

## Test

1. Build and install the ARM64 APK.
2. Open the app.
3. The large SurfaceView should initialize automatically.
4. A colored Vulkan frame should be presented.
5. Tap **Present frame** repeatedly.
6. The color should change each time.

A successful status resembles:

```
Presented frame N directly through Vulkan (WIDTHxHEIGHT)
```

## What this proves

- Java Surface -> ANativeWindow works.
- Android Vulkan surface creation works.
- The host owns a swapchain.
- GPU presentation can happen without capture permissions.
- The future frame-generation compositor has a valid output target.

## What it does not prove yet

It does not access frames from another app.

That is M1/M2:

```
virtualized guest
  -> graphics interception
  -> guest frame
  -> frame bridge
  -> this compositor
```

## Next technical milestone

Build the graphics interception API separately from the frame-generation backend.

The first interceptor should record and proxy a controlled test application's calls around:

- vkGetInstanceProcAddr
- vkGetDeviceProcAddr
- vkCreateAndroidSurfaceKHR
- vkCreateSwapchainKHR
- vkGetSwapchainImagesKHR
- vkAcquireNextImageKHR
- vkQueuePresentKHR

The first target must be a test guest that we control, not a commercial game.
