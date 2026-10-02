# M0 - Native Vulkan Probe

This milestone intentionally does not implement frame generation yet.

Its purpose is to validate the Android/NDK/Vulkan build path on real ARM64 hardware.

## Success criteria

Install the APK, open it and tap **Probe Vulkan**.

A successful result should display:

- Vulkan OK
- GPU name
- Vulkan API version
- driver version
- number of exposed physical devices

## Why this comes first

The eventual interceptor, frame bridge and compositor will all live in native code.

Before adding app virtualization, we need a known-good native Vulkan baseline that can be built, installed and tested independently.

## Next M0 step

Replace the probe-only screen with a host-owned `SurfaceView` and create a real Vulkan Android surface/swapchain from its `ANativeWindow`.

That becomes the output surface for the future compositor.
