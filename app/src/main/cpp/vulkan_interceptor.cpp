#include <jni.h>
#include <vulkan/vulkan.h>
#include <bytehook.h>

#include <atomic>
#include <cstring>
#include <sstream>
#include <string>

namespace {

std::atomic<uint64_t> gGetInstanceProcAddrCalls{0};
std::atomic<uint64_t> gGetDeviceProcAddrCalls{0};
std::atomic<uint64_t> gAcquireCalls{0};
std::atomic<uint64_t> gPresentCalls{0};
std::atomic<uint64_t> gCreateSwapchainCalls{0};

std::atomic<PFN_vkQueuePresentKHR> gRealQueuePresent{nullptr};
std::atomic<PFN_vkCreateSwapchainKHR> gRealCreateSwapchain{nullptr};
std::atomic<PFN_vkAcquireNextImageKHR> gRealAcquireNextImage{nullptr};

bytehook_stub_t gGipaStub = nullptr;
bytehook_stub_t gGdpaStub = nullptr;
bytehook_stub_t gAcquireStub = nullptr;
bytehook_stub_t gPresentStub = nullptr;
bytehook_stub_t gCreateSwapchainStub = nullptr;

jstring toJString(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

bool guestLibraryFilter(const char* callerPathName, void*) {
    if (callerPathName == nullptr) {
        return false;
    }

    return std::strstr(
            callerPathName,
            "libframegen_guest_test.so") != nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL observedQueuePresent(
        VkQueue queue,
        const VkPresentInfoKHR* presentInfo) {
    gPresentCalls.fetch_add(1, std::memory_order_relaxed);

    PFN_vkQueuePresentKHR real =
            gRealQueuePresent.load(std::memory_order_acquire);
    if (real == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return real(queue, presentInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL observedCreateSwapchain(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* createInfo,
        const VkAllocationCallbacks* allocator,
        VkSwapchainKHR* swapchain) {
    gCreateSwapchainCalls.fetch_add(1, std::memory_order_relaxed);

    PFN_vkCreateSwapchainKHR real =
            gRealCreateSwapchain.load(std::memory_order_acquire);
    if (real == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return real(device, createInfo, allocator, swapchain);
}

VKAPI_ATTR VkResult VKAPI_CALL observedAcquireNextImage(
        VkDevice device,
        VkSwapchainKHR swapchain,
        uint64_t timeout,
        VkSemaphore semaphore,
        VkFence fence,
        uint32_t* imageIndex) {
    gAcquireCalls.fetch_add(1, std::memory_order_relaxed);

    PFN_vkAcquireNextImageKHR real =
            gRealAcquireNextImage.load(std::memory_order_acquire);
    if (real == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return real(device, swapchain, timeout, semaphore, fence, imageIndex);
}

PFN_vkVoidFunction proxyVkGetInstanceProcAddr(
        VkInstance instance,
        const char* name) {
    BYTEHOOK_STACK_SCOPE();
    gGetInstanceProcAddrCalls.fetch_add(1, std::memory_order_relaxed);
    return BYTEHOOK_CALL_PREV(
            proxyVkGetInstanceProcAddr,
            instance,
            name);
}

PFN_vkVoidFunction proxyVkGetDeviceProcAddr(
        VkDevice device,
        const char* name) {
    BYTEHOOK_STACK_SCOPE();
    gGetDeviceProcAddrCalls.fetch_add(1, std::memory_order_relaxed);

    PFN_vkVoidFunction real =
            BYTEHOOK_CALL_PREV(
                    proxyVkGetDeviceProcAddr,
                    device,
                    name);

    if (real == nullptr || name == nullptr) {
        return real;
    }

    if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
        gRealQueuePresent.store(
                reinterpret_cast<PFN_vkQueuePresentKHR>(real),
                std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(
                observedQueuePresent);
    }

    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0) {
        gRealCreateSwapchain.store(
                reinterpret_cast<PFN_vkCreateSwapchainKHR>(real),
                std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(
                observedCreateSwapchain);
    }

    if (std::strcmp(name, "vkAcquireNextImageKHR") == 0) {
        gRealAcquireNextImage.store(
                reinterpret_cast<PFN_vkAcquireNextImageKHR>(real),
                std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(
                observedAcquireNextImage);
    }

    return real;
}

VKAPI_ATTR VkResult VKAPI_CALL proxyVkQueuePresentKHR(
        VkQueue queue,
        const VkPresentInfoKHR* presentInfo) {
    BYTEHOOK_STACK_SCOPE();
    gPresentCalls.fetch_add(1, std::memory_order_relaxed);
    return BYTEHOOK_CALL_PREV(
            proxyVkQueuePresentKHR,
            queue,
            presentInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL proxyVkCreateSwapchainKHR(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* createInfo,
        const VkAllocationCallbacks* allocator,
        VkSwapchainKHR* swapchain) {
    BYTEHOOK_STACK_SCOPE();
    gCreateSwapchainCalls.fetch_add(
            1,
            std::memory_order_relaxed);
    return BYTEHOOK_CALL_PREV(
            proxyVkCreateSwapchainKHR,
            device,
            createInfo,
            allocator,
            swapchain);
}

VKAPI_ATTR VkResult VKAPI_CALL proxyVkAcquireNextImageKHR(
        VkDevice device,
        VkSwapchainKHR swapchain,
        uint64_t timeout,
        VkSemaphore semaphore,
        VkFence fence,
        uint32_t* imageIndex) {
    BYTEHOOK_STACK_SCOPE();
    gAcquireCalls.fetch_add(1, std::memory_order_relaxed);
    return BYTEHOOK_CALL_PREV(
            proxyVkAcquireNextImageKHR,
            device,
            swapchain,
            timeout,
            semaphore,
            fence,
            imageIndex);
}

std::string stats() {
    std::ostringstream out;
    out << "Guest Vulkan interception\n"
        << "vkGetInstanceProcAddr: "
        << gGetInstanceProcAddrCalls.load(
                std::memory_order_relaxed) << "\n"
        << "vkGetDeviceProcAddr: "
        << gGetDeviceProcAddrCalls.load(
                std::memory_order_relaxed) << "\n"
        << "vkCreateSwapchainKHR: "
        << gCreateSwapchainCalls.load(
                std::memory_order_relaxed) << "\n"
        << "vkAcquireNextImageKHR: "
        << gAcquireCalls.load(
                std::memory_order_relaxed) << "\n"
        << "vkQueuePresentKHR: "
        << gPresentCalls.load(
                std::memory_order_relaxed);
    return out.str();
}

} // namespace

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeInstallVulkanHooks(
        JNIEnv* env,
        jclass) {
    if (gGipaStub != nullptr || gGdpaStub != nullptr) {
        return toJString(
                env,
                "Vulkan guest hooks already installed.\n" + stats());
    }

    const int initResult =
            bytehook_init(BYTEHOOK_MODE_AUTOMATIC, false);
    if (initResult != BYTEHOOK_STATUS_CODE_OK) {
        std::ostringstream out;
        out << "ByteHook init failed. status=" << initResult;
        return toJString(env, out.str());
    }

    gGipaStub = bytehook_hook_partial(
            guestLibraryFilter,
            nullptr,
            "libvulkan.so",
            "vkGetInstanceProcAddr",
            reinterpret_cast<void*>(
                    proxyVkGetInstanceProcAddr),
            nullptr,
            nullptr);

    gGdpaStub = bytehook_hook_partial(
            guestLibraryFilter,
            nullptr,
            "libvulkan.so",
            "vkGetDeviceProcAddr",
            reinterpret_cast<void*>(
                    proxyVkGetDeviceProcAddr),
            nullptr,
            nullptr);

    gCreateSwapchainStub = bytehook_hook_partial(
            guestLibraryFilter,
            nullptr,
            "libvulkan.so",
            "vkCreateSwapchainKHR",
            reinterpret_cast<void*>(
                    proxyVkCreateSwapchainKHR),
            nullptr,
            nullptr);

    gAcquireStub = bytehook_hook_partial(
            guestLibraryFilter,
            nullptr,
            "libvulkan.so",
            "vkAcquireNextImageKHR",
            reinterpret_cast<void*>(
                    proxyVkAcquireNextImageKHR),
            nullptr,
            nullptr);

    gPresentStub = bytehook_hook_partial(
            guestLibraryFilter,
            nullptr,
            "libvulkan.so",
            "vkQueuePresentKHR",
            reinterpret_cast<void*>(
                    proxyVkQueuePresentKHR),
            nullptr,
            nullptr);

    if (gGipaStub == nullptr || gGdpaStub == nullptr ||
        gCreateSwapchainStub == nullptr ||
        gAcquireStub == nullptr ||
        gPresentStub == nullptr) {
        return toJString(
                env,
                "One or more Vulkan hook tasks could not be created.");
    }

    return toJString(
            env,
            "Guest Vulkan hooks armed. "
            "Only libframegen_guest_test.so is allowed.");
}

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeGetVulkanHookStats(
        JNIEnv* env,
        jclass) {
    return toJString(env, stats());
}
