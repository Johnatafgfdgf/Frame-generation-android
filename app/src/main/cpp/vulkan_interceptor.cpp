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
std::atomic<uint64_t> gPresentCalls{0};
std::atomic<uint64_t> gCreateSwapchainCalls{0};

std::atomic<PFN_vkQueuePresentKHR> gRealQueuePresent{nullptr};
std::atomic<PFN_vkCreateSwapchainKHR> gRealCreateSwapchain{nullptr};

bytehook_stub_t gGipaStub = nullptr;
bytehook_stub_t gGdpaStub = nullptr;
bytehook_stub_t gPresentStub = nullptr;
bytehook_stub_t gCreateSwapchainStub = nullptr;

jstring toJString(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

bool guestLibraryFilter(const char* callerPathName, void*) {
    if (callerPathName == nullptr) {
        return false;
    }

    // M1A deliberately hooks only our controlled guest test library.
    // Later the virtual runtime will replace this with the guest library set.
    return std::strstr(callerPathName, "libframegen_guest_test.so") != nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL observedQueuePresent(
        VkQueue queue,
        const VkPresentInfoKHR* presentInfo) {
    gPresentCalls.fetch_add(1, std::memory_order_relaxed);

    PFN_vkQueuePresentKHR real = gRealQueuePresent.load(std::memory_order_acquire);
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

PFN_vkVoidFunction proxyVkGetInstanceProcAddr(
        VkInstance instance,
        const char* name) {
    BYTEHOOK_STACK_SCOPE();
    gGetInstanceProcAddrCalls.fetch_add(1, std::memory_order_relaxed);

    return BYTEHOOK_CALL_PREV(proxyVkGetInstanceProcAddr, instance, name);
}

PFN_vkVoidFunction proxyVkGetDeviceProcAddr(
        VkDevice device,
        const char* name) {
    BYTEHOOK_STACK_SCOPE();
    gGetDeviceProcAddrCalls.fetch_add(1, std::memory_order_relaxed);

    PFN_vkVoidFunction real =
            BYTEHOOK_CALL_PREV(proxyVkGetDeviceProcAddr, device, name);

    if (real == nullptr || name == nullptr) {
        return real;
    }

    if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
        gRealQueuePresent.store(
                reinterpret_cast<PFN_vkQueuePresentKHR>(real),
                std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(observedQueuePresent);
    }

    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0) {
        gRealCreateSwapchain.store(
                reinterpret_cast<PFN_vkCreateSwapchainKHR>(real),
                std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(observedCreateSwapchain);
    }

    return real;
}

VKAPI_ATTR VkResult VKAPI_CALL proxyVkQueuePresentKHR(
        VkQueue queue,
        const VkPresentInfoKHR* presentInfo) {
    BYTEHOOK_STACK_SCOPE();
    gPresentCalls.fetch_add(1, std::memory_order_relaxed);
    return BYTEHOOK_CALL_PREV(proxyVkQueuePresentKHR, queue, presentInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL proxyVkCreateSwapchainKHR(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* createInfo,
        const VkAllocationCallbacks* allocator,
        VkSwapchainKHR* swapchain) {
    BYTEHOOK_STACK_SCOPE();
    gCreateSwapchainCalls.fetch_add(1, std::memory_order_relaxed);
    return BYTEHOOK_CALL_PREV(
            proxyVkCreateSwapchainKHR,
            device,
            createInfo,
            allocator,
            swapchain);
}

std::string stats() {
    std::ostringstream out;
    out << "Guest Vulkan interception\n"
        << "vkGetInstanceProcAddr: "
        << gGetInstanceProcAddrCalls.load(std::memory_order_relaxed) << "\n"
        << "vkGetDeviceProcAddr: "
        << gGetDeviceProcAddrCalls.load(std::memory_order_relaxed) << "\n"
        << "vkCreateSwapchainKHR: "
        << gCreateSwapchainCalls.load(std::memory_order_relaxed) << "\n"
        << "vkQueuePresentKHR: "
        << gPresentCalls.load(std::memory_order_relaxed);
    return out.str();
}

} // namespace

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeInstallVulkanHooks(
        JNIEnv* env,
        jclass) {
    if (gGipaStub != nullptr || gGdpaStub != nullptr) {
        return toJString(env, "Vulkan guest hooks already installed.\n" + stats());
    }

    const int initResult = bytehook_init(BYTEHOOK_MODE_AUTOMATIC, false);
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
            reinterpret_cast<void*>(proxyVkGetInstanceProcAddr),
            nullptr,
            nullptr);

    gGdpaStub = bytehook_hook_partial(
            guestLibraryFilter,
            nullptr,
            "libvulkan.so",
            "vkGetDeviceProcAddr",
            reinterpret_cast<void*>(proxyVkGetDeviceProcAddr),
            nullptr,
            nullptr);

    // Cover games that import these functions directly instead of resolving
    // them only through vkGetDeviceProcAddr.
    gPresentStub = bytehook_hook_partial(
            guestLibraryFilter,
            nullptr,
            "libvulkan.so",
            "vkQueuePresentKHR",
            reinterpret_cast<void*>(proxyVkQueuePresentKHR),
            nullptr,
            nullptr);

    gCreateSwapchainStub = bytehook_hook_partial(
            guestLibraryFilter,
            nullptr,
            "libvulkan.so",
            "vkCreateSwapchainKHR",
            reinterpret_cast<void*>(proxyVkCreateSwapchainKHR),
            nullptr,
            nullptr);

    if (gGipaStub == nullptr || gGdpaStub == nullptr) {
        return toJString(
                env,
                "Hook task creation failed. GIPA/GDPA must both be installed.");
    }

    return toJString(
            env,
            "M1A hooks armed for libframegen_guest_test.so. "
            "Load the guest test next.");
}

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeGetVulkanHookStats(
        JNIEnv* env,
        jclass) {
    return toJString(env, stats());
}
