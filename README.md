# Frame-generation-android

Experimental Android graphics runtime for frame generation without MediaProjection.

## Goal

Run an Android app inside a host-controlled environment and intercept its graphics path before presentation, allowing frame generation and post-processing without capturing the screen.

Target path:

```
Guest APK
  -> virtualized runtime
  -> Vulkan / EGL interceptor
  -> frame bridge
  -> frame-generation backend
  -> host compositor
  -> visible Android Surface
```

The key idea is that the host owns the rendering boundary. We do not try to capture another normal Android app after it has already rendered.

## Status

Research / architecture phase.

First proof of concept:

1. Host Android app with a native Vulkan module.
2. Controlled guest process.
3. Detect Vulkan swapchain creation and presentation.
4. Copy a rendered frame GPU-to-GPU into a host-owned buffer.
5. Present it through our own compositor.
6. Only after that, add a frame-generation backend.

## Non-goals for the first milestone

- Full Android VM.
- MediaProjection capture.
- Root requirement.
- Shipping proprietary Lossless Scaling assets.
- Bypassing DRM or anti-cheat.

## Project principles

- Keep app virtualization, graphics interception, frame generation and presentation as separate modules.
- Prefer zero-copy GPU paths using Vulkan external memory / AHardwareBuffer where possible.
- Keep the frame-generation backend replaceable.
- Start with Vulkan; add EGL/OpenGL ES later.
- Test on real ARM64 Android hardware first.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/REFERENCES.md](docs/REFERENCES.md).

## License

Project license is intentionally not chosen yet.

Third-party code must retain its own license and attribution. Do not copy code into this repository until the license of the exact source/version has been reviewed.
