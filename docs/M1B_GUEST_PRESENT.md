# M1B - Intercept a real guest swapchain/present

M1B upgrades the controlled guest from Vulkan function lookup to a real
Android Vulkan presentation path.

## Layout

The app now has two independent Surfaces:

```
Host SurfaceView
  -> host compositor
  -> host VkSwapchainKHR

Guest SurfaceView
  -> libframegen_guest_test.so
  -> guest VkSwapchainKHR
  -> intercepted vkAcquireNextImageKHR
  -> intercepted vkQueuePresentKHR
```

The guest library is still loaded only after the guest-only hook tasks are
armed.

## Expected result

Tap **M1B guest present**.

A colored frame should appear in the guest SurfaceView.

The statistics should increase for at least:

```
vkCreateSwapchainKHR: 1+
vkAcquireNextImageKHR: 1+
vkQueuePresentKHR: 1+
```

No MediaProjection permission is involved.

## Why this matters

At this point the host can observe the exact guest presentation boundary.

The next milestone no longer needs to prove interception. It can focus on
accessing/copying the acquired guest swapchain image before presentation.

## M2 target

Track, per guest swapchain:

- VkDevice
- VkSwapchainKHR
- VkFormat
- VkExtent2D
- swapchain VkImage handles
- acquired image index
- presentation queue

Then add a GPU-only copy path:

```
guest swapchain image
  -> GPU copy / shared image
  -> host-owned frame bridge
  -> host compositor
```

The first M2 implementation should copy the real guest frame unchanged.
Frame interpolation comes only after that bridge is stable.
