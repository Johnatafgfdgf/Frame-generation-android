# M1A - Vulkan interception proof

This milestone tests native Vulkan interception inside a process we control.

It does **not** yet virtualize a third-party APK.

## Why ByteHook

ByteHook is an MIT-licensed Android PLT hook library with ARM64 support.

For this proof, the host installs hook tasks before loading
`libframegen_guest_test.so`.

The filter only accepts that controlled test library:

```
libframegen_guest_test.so
    -> PLT call: vkGetInstanceProcAddr
    -> ByteHook
    -> proxyVkGetInstanceProcAddr

libframegen_guest_test.so
    -> PLT call: vkGetDeviceProcAddr
    -> ByteHook
    -> proxyVkGetDeviceProcAddr
```

The host compositor library is deliberately excluded.

## Function-pointer interception

Many Vulkan applications obtain device functions through
`vkGetDeviceProcAddr` rather than importing every Vulkan command directly.

M1A therefore also replaces returned function pointers for:

- `vkQueuePresentKHR`
- `vkCreateSwapchainKHR`

The returned wrapper records the call and forwards it to the real driver
function.

This is the mechanism we will eventually use to place the frame bridge around
presentation.

## Direct-import interception

Separate ByteHook tasks are also installed for direct PLT imports of:

- `vkQueuePresentKHR`
- `vkCreateSwapchainKHR`

This lets the interceptor support both common loading styles.

## Test procedure

Tap **Run M1A Vulkan hook test**.

The app:

1. arms hooks for the guest test library;
2. loads the guest library only after hooks exist;
3. creates a Vulkan instance and device from the guest;
4. calls `vkGetInstanceProcAddr`;
5. calls `vkGetDeviceProcAddr` for swapchain/present functions;
6. prints interception counters.

Expected minimum result after one test:

```
vkGetInstanceProcAddr: >= 1
vkGetDeviceProcAddr: >= 2
```

The swapchain/present call counters remain zero until those functions are
actually invoked.

## Next step: M1B

M1B will give the controlled guest its own Android Surface and make it create
a real swapchain.

Then the interceptor can observe:

```
vkCreateSwapchainKHR
vkAcquireNextImageKHR
vkQueuePresentKHR
```

Once that works, M2 can redirect/copy the presented image into the host
compositor without MediaProjection.
