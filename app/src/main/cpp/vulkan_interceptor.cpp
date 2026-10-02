#include <jni.h>
#include <vulkan/vulkan.h>
#include <bytehook.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct SwapchainRecord {
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    uint32_t minImageCount = 0;

    std::vector<VkImage> images;

    uint32_t lastAcquiredIndex = 0;
    bool hasAcquiredIndex = false;

    VkQueue lastPresentQueue = VK_NULL_HANDLE;
    uint64_t presentCount = 0;
};

struct PresentedSnapshot {
    bool valid = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    uint32_t imageCount = 0;
    uint32_t imageIndex = 0;
    uint64_t presentSerial = 0;
};

std::atomic<uint64_t> gGetInstanceProcAddrCalls{0};
std::atomic<uint64_t> gGetDeviceProcAddrCalls{0};
std::atomic<uint64_t> gCreateSwapchainCalls{0};
std::atomic<uint64_t> gDestroySwapchainCalls{0};
std::atomic<uint64_t> gGetSwapchainImagesCalls{0};
std::atomic<uint64_t> gAcquireCalls{0};
std::atomic<uint64_t> gPresentCalls{0};
std::atomic<uint64_t> gPresentSerial{0};

std::atomic<PFN_vkCreateSwapchainKHR> gRealCreateSwapchain{nullptr};
std::atomic<PFN_vkDestroySwapchainKHR> gRealDestroySwapchain{nullptr};
std::atomic<PFN_vkGetSwapchainImagesKHR> gRealGetSwapchainImages{nullptr};
std::atomic<PFN_vkAcquireNextImageKHR> gRealAcquireNextImage{nullptr};
std::atomic<PFN_vkQueuePresentKHR> gRealQueuePresent{nullptr};

bytehook_stub_t gGipaStub = nullptr;
bytehook_stub_t gGdpaStub = nullptr;
bytehook_stub_t gCreateSwapchainStub = nullptr;
bytehook_stub_t gDestroySwapchainStub = nullptr;
bytehook_stub_t gGetSwapchainImagesStub = nullptr;
bytehook_stub_t gAcquireStub = nullptr;
bytehook_stub_t gPresentStub = nullptr;

std::mutex gRegistryMutex;
std::unordered_map<VkSwapchainKHR, SwapchainRecord> gSwapchains;
PresentedSnapshot gLastPresented;

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

void recordCreate(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* createInfo,
        VkSwapchainKHR swapchain,
        VkResult result) {
    if (result != VK_SUCCESS ||
        createInfo == nullptr ||
        swapchain == VK_NULL_HANDLE) {
        return;
    }

    SwapchainRecord record{};
    record.device = device;
    record.format = createInfo->imageFormat;
    record.extent = createInfo->imageExtent;
    record.usage = createInfo->imageUsage;
    record.minImageCount = createInfo->minImageCount;

    std::lock_guard<std::mutex> lock(gRegistryMutex);
    gSwapchains[swapchain] = std::move(record);
}

void recordImages(
        VkSwapchainKHR swapchain,
        uint32_t imageCount,
        const VkImage* images,
        VkResult result) {
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
        images == nullptr ||
        imageCount == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(gRegistryMutex);
    auto it = gSwapchains.find(swapchain);
    if (it == gSwapchains.end()) {
        return;
    }

    it->second.images.assign(images, images + imageCount);
}

void recordAcquire(
        VkSwapchainKHR swapchain,
        const uint32_t* imageIndex,
        VkResult result) {
    if ((result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) ||
        imageIndex == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(gRegistryMutex);
    auto it = gSwapchains.find(swapchain);
    if (it == gSwapchains.end()) {
        return;
    }

    it->second.lastAcquiredIndex = *imageIndex;
    it->second.hasAcquiredIndex = true;
}

void recordPresent(
        VkQueue queue,
        const VkPresentInfoKHR* presentInfo,
        VkResult result) {
    if ((result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) ||
        presentInfo == nullptr ||
        presentInfo->pSwapchains == nullptr ||
        presentInfo->swapchainCount == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(gRegistryMutex);

    for (uint32_t i = 0; i < presentInfo->swapchainCount; ++i) {
        VkSwapchainKHR swapchain = presentInfo->pSwapchains[i];
        auto it = gSwapchains.find(swapchain);
        if (it == gSwapchains.end()) {
            continue;
        }

        SwapchainRecord& record = it->second;
        record.lastPresentQueue = queue;
        record.presentCount++;

        uint32_t imageIndex = record.lastAcquiredIndex;
        if (presentInfo->pImageIndices != nullptr) {
            imageIndex = presentInfo->pImageIndices[i];
        }

        gLastPresented.valid = true;
        gLastPresented.format = record.format;
        gLastPresented.extent = record.extent;
        gLastPresented.usage = record.usage;
        gLastPresented.imageCount =
                static_cast<uint32_t>(record.images.size());
        gLastPresented.imageIndex = imageIndex;
        gLastPresented.presentSerial =
                gPresentSerial.fetch_add(
                        1,
                        std::memory_order_relaxed) + 1;
    }
}

void recordDestroy(VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    gSwapchains.erase(swapchain);
}

std::string registrySummary() {
    std::lock_guard<std::mutex> lock(gRegistryMutex);

    std::ostringstream out;
    out << "M2 swapchain registry\n"
        << "Active swapchains: " << gSwapchains.size() << "\n";

    size_t ordinal = 0;
    for (const auto& entry : gSwapchains) {
        const SwapchainRecord& record = entry.second;

        out << "#" << (++ordinal)
            << " " << record.extent.width
            << "x" << record.extent.height
            << " format=" << static_cast<int>(record.format)
            << " images=" << record.images.size()
            << " usage=0x" << std::hex << record.usage << std::dec;

        if (record.hasAcquiredIndex) {
            out << " acquired=" << record.lastAcquiredIndex;
        }

        out << " presents=" << record.presentCount << "\n";
    }

    if (gLastPresented.valid) {
        out << "Last presented frame: "
            << gLastPresented.extent.width
            << "x" << gLastPresented.extent.height
            << " image=" << gLastPresented.imageIndex
            << "/" << gLastPresented.imageCount
            << " format=" << static_cast<int>(gLastPresented.format)
            << " serial=" << gLastPresented.presentSerial;
    } else {
        out << "Last presented frame: none yet";
    }

    return out.str();
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

    VkResult result = real(device, createInfo, allocator, swapchain);
    if (swapchain != nullptr) {
        recordCreate(device, createInfo, *swapchain, result);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL observedDestroySwapchain(
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* allocator) {
    gDestroySwapchainCalls.fetch_add(1, std::memory_order_relaxed);

    PFN_vkDestroySwapchainKHR real =
            gRealDestroySwapchain.load(std::memory_order_acquire);

    recordDestroy(swapchain);
    if (real != nullptr) {
        real(device, swapchain, allocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL observedGetSwapchainImages(
        VkDevice device,
        VkSwapchainKHR swapchain,
        uint32_t* imageCount,
        VkImage* images) {
    gGetSwapchainImagesCalls.fetch_add(1, std::memory_order_relaxed);

    PFN_vkGetSwapchainImagesKHR real =
            gRealGetSwapchainImages.load(std::memory_order_acquire);
    if (real == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = real(device, swapchain, imageCount, images);
    if (imageCount != nullptr && images != nullptr) {
        recordImages(swapchain, *imageCount, images, result);
    }
    return result;
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

    VkResult result = real(
            device,
            swapchain,
            timeout,
            semaphore,
            fence,
            imageIndex);
    recordAcquire(swapchain, imageIndex, result);
    return result;
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

    VkResult result = real(queue, presentInfo);
    recordPresent(queue, presentInfo, result);
    return result;
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

    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0) {
        gRealCreateSwapchain.store(
                reinterpret_cast<PFN_vkCreateSwapchainKHR>(real),
                std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(
                observedCreateSwapchain);
    }

    if (std::strcmp(name, "vkDestroySwapchainKHR") == 0) {
        gRealDestroySwapchain.store(
                reinterpret_cast<PFN_vkDestroySwapchainKHR>(real),
                std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(
                observedDestroySwapchain);
    }

    if (std::strcmp(name, "vkGetSwapchainImagesKHR") == 0) {
        gRealGetSwapchainImages.store(
                reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(real),
                std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(
                observedGetSwapchainImages);
    }

    if (std::strcmp(name, "vkAcquireNextImageKHR") == 0) {
        gRealAcquireNextImage.store(
                reinterpret_cast<PFN_vkAcquireNextImageKHR>(real),
                std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(
                observedAcquireNextImage);
    }

    if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
        gRealQueuePresent.store(
                reinterpret_cast<PFN_vkQueuePresentKHR>(real),
                std::memory_order_release);
        return reinterpret_cast<PFN_vkVoidFunction>(
                observedQueuePresent);
    }

    return real;
}

VKAPI_ATTR VkResult VKAPI_CALL proxyVkCreateSwapchainKHR(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* createInfo,
        const VkAllocationCallbacks* allocator,
        VkSwapchainKHR* swapchain) {
    BYTEHOOK_STACK_SCOPE();
    gCreateSwapchainCalls.fetch_add(1, std::memory_order_relaxed);

    VkResult result = BYTEHOOK_CALL_PREV(
            proxyVkCreateSwapchainKHR,
            device,
            createInfo,
            allocator,
            swapchain);

    if (swapchain != nullptr) {
        recordCreate(device, createInfo, *swapchain, result);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL proxyVkDestroySwapchainKHR(
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* allocator) {
    BYTEHOOK_STACK_SCOPE();
    gDestroySwapchainCalls.fetch_add(1, std::memory_order_relaxed);

    recordDestroy(swapchain);
    BYTEHOOK_CALL_PREV(
            proxyVkDestroySwapchainKHR,
            device,
            swapchain,
            allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL proxyVkGetSwapchainImagesKHR(
        VkDevice device,
        VkSwapchainKHR swapchain,
        uint32_t* imageCount,
        VkImage* images) {
    BYTEHOOK_STACK_SCOPE();
    gGetSwapchainImagesCalls.fetch_add(1, std::memory_order_relaxed);

    VkResult result = BYTEHOOK_CALL_PREV(
            proxyVkGetSwapchainImagesKHR,
            device,
            swapchain,
            imageCount,
            images);

    if (imageCount != nullptr && images != nullptr) {
        recordImages(swapchain, *imageCount, images, result);
    }
    return result;
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

    VkResult result = BYTEHOOK_CALL_PREV(
            proxyVkAcquireNextImageKHR,
            device,
            swapchain,
            timeout,
            semaphore,
            fence,
            imageIndex);

    recordAcquire(swapchain, imageIndex, result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL proxyVkQueuePresentKHR(
        VkQueue queue,
        const VkPresentInfoKHR* presentInfo) {
    BYTEHOOK_STACK_SCOPE();
    gPresentCalls.fetch_add(1, std::memory_order_relaxed);

    VkResult result = BYTEHOOK_CALL_PREV(
            proxyVkQueuePresentKHR,
            queue,
            presentInfo);

    recordPresent(queue, presentInfo, result);
    return result;
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
        << "vkGetSwapchainImagesKHR: "
        << gGetSwapchainImagesCalls.load(
                std::memory_order_relaxed) << "\n"
        << "vkAcquireNextImageKHR: "
        << gAcquireCalls.load(
                std::memory_order_relaxed) << "\n"
        << "vkQueuePresentKHR: "
        << gPresentCalls.load(
                std::memory_order_relaxed) << "\n"
        << "vkDestroySwapchainKHR: "
        << gDestroySwapchainCalls.load(
                std::memory_order_relaxed);
    return out.str();
}

bytehook_stub_t installGuestHook(
        const char* symbol,
        void* proxy) {
    return bytehook_hook_partial(
            guestLibraryFilter,
            nullptr,
            "libvulkan.so",
            symbol,
            proxy,
            nullptr,
            nullptr);
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

    gGipaStub = installGuestHook(
            "vkGetInstanceProcAddr",
            reinterpret_cast<void*>(proxyVkGetInstanceProcAddr));

    gGdpaStub = installGuestHook(
            "vkGetDeviceProcAddr",
            reinterpret_cast<void*>(proxyVkGetDeviceProcAddr));

    gCreateSwapchainStub = installGuestHook(
            "vkCreateSwapchainKHR",
            reinterpret_cast<void*>(proxyVkCreateSwapchainKHR));

    gDestroySwapchainStub = installGuestHook(
            "vkDestroySwapchainKHR",
            reinterpret_cast<void*>(proxyVkDestroySwapchainKHR));

    gGetSwapchainImagesStub = installGuestHook(
            "vkGetSwapchainImagesKHR",
            reinterpret_cast<void*>(proxyVkGetSwapchainImagesKHR));

    gAcquireStub = installGuestHook(
            "vkAcquireNextImageKHR",
            reinterpret_cast<void*>(proxyVkAcquireNextImageKHR));

    gPresentStub = installGuestHook(
            "vkQueuePresentKHR",
            reinterpret_cast<void*>(proxyVkQueuePresentKHR));

    if (gGipaStub == nullptr ||
        gGdpaStub == nullptr ||
        gCreateSwapchainStub == nullptr ||
        gDestroySwapchainStub == nullptr ||
        gGetSwapchainImagesStub == nullptr ||
        gAcquireStub == nullptr ||
        gPresentStub == nullptr) {
        return toJString(
                env,
                "One or more Vulkan hook tasks could not be created.");
    }

    return toJString(
            env,
            "M2 guest Vulkan hooks armed. "
            "Swapchain state and presented image index will be tracked.");
}

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeGetVulkanHookStats(
        JNIEnv* env,
        jclass) {
    return toJString(env, stats());
}

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeGetSwapchainRegistry(
        JNIEnv* env,
        jclass) {
    return toJString(env, registrySummary());
}
