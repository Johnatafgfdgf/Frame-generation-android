# References

These projects are references, not automatic dependencies.

## VirtualApp

Purpose for this project:
- guest APK loading;
- Android service/package virtualization;
- controlled stub processes;
- examples of native hooking inside a virtualized app environment.

Modern Android forks exist with Android 14 fixes.

Important: review the exact fork and license before importing code.

## gfxstream

Purpose for this project:
- architecture reference for passing graphics API commands/resources between guest and host;
- ideas for separating guest encoder/transport from host renderer.

It may be too large for the first implementation, but its architecture is relevant if direct interception becomes fragile.

## Vulkan Layers

Vulkan layers demonstrate the ideal interception model: a dispatch layer between application and driver.

On normal non-debuggable Android apps, we cannot rely on installing a system-wide implicit layer. Our virtual runtime therefore needs to establish an equivalent interception point inside a process that we control.

## LSFG / LSFG-Android

Useful concepts:
- intercepting presentation;
- frame-generation pipeline organization;
- AHardwareBuffer-based Android image sharing experiments;
- pacing and generated-frame presentation.

### Licensing note

The current lsfg-vk upstream was relicensed in August 2026 to CC BY-NC-ND 4.0, which prohibits derivative works without permission.

Therefore:
- do not copy current upstream source into this repository;
- treat it as an architectural reference unless explicit permission allows integration;
- verify the license of any historical commit/tag before reuse;
- do not redistribute Lossless Scaling proprietary assets.

The project should keep its own graphics interception and compositor independent of any specific frame-generation implementation.

## Android Virtualization Framework

AVF is important background for true Android virtualization, but it is oriented toward protected/isolated virtual machines and is not the first target for a lightweight phone-side gaming runtime.

## Initial implementation choice

Start with:

```
Virtualized app process
  + custom graphics interceptor
  + GPU frame bridge
  + host compositor
```

Only investigate a true guest VM if this path proves technically impossible or too unstable.
