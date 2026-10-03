#include <jni.h>
#include <vulkan/vulkan.h>
#include <bytehook.h>

#include "guest_frame_bridge.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
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

struct CaptureInfo {
    bool valid = false;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
};

std::atomic<uint64_t> gGetInstanceProcAddrCalls{0};
std::atomic<uint64_t> gGetDeviceProcAddrCalls{0};
std::atomic<uint64_t> gCreateDeviceCalls{0};
std::atomic<uint64_t> gDestroyDeviceCalls{0};
std::atomic<uint64_t> gGetDeviceQueueCalls{0};
std::atomic<uint64_t> gCreateSwapchainCalls{0};
std::atomic<uint64_t> gDestroySwapchainCalls{0};
std::atomic<uint64_t> gGetSwapchainImagesCalls{0};
std::atomic<uint64_t> gAcquireCalls{0};
std::atomic<uint64_t> gPresentCalls{0};
std::atomic<uint64_t> gPresentSerial{0};
std::atomic<bool> gGuestBridgeEnabled{false};

std::atomic<PFN_vkCreateDevice> gRealCreateDevice{nullptr};
std::atomic<PFN_vkDestroyDevice> gRealDestroyDevice{nullptr};
std::atomic<PFN_vkGetDeviceQueue> gRealGetDeviceQueue{nullptr};
std::atomic<PFN_vkGetDeviceQueue2> gRealGetDeviceQueue2{nullptr};
std::atomic<PFN_vkCreateSwapchainKHR> gRealCreateSwapchain{nullptr};
std::atomic<PFN_vkDestroySwapchainKHR> gRealDestroySwapchain{nullptr};
std::atomic<PFN_vkGetSwapchainImagesKHR> gRealGetSwapchainImages{nullptr};
std::atomic<PFN_vkAcquireNextImageKHR> gRealAcquireNextImage{nullptr};
std::atomic<PFN_vkQueuePresentKHR> gRealQueuePresent{nullptr};

bytehook_stub_t gGipaStub = nullptr;
bytehook_stub_t gGdpaStub = nullptr;
bytehook_stub_t gCreateDeviceStub = nullptr;
bytehook_stub_t gDestroyDeviceStub = nullptr;
bytehook_stub_t gGetDeviceQueueStub = nullptr;
bytehook_stub_t gGetDeviceQueue2Stub = nullptr;
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

bool deviceSupportsExtension(
        VkPhysicalDevice physicalDevice,
        const char* extensionName) {
    uint32_t count = 0;

    if (vkEnumerateDeviceExtensionProperties(
            physicalDevice,
            nullptr,
            &count,
            nullptr) != VK_SUCCESS) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(count);

    if (vkEnumerateDeviceExtensionProperties(
            physicalDevice,
            nullptr,
            &count,
            extensions.data()) != VK_SUCCESS) {
        return false;
    }

    for (const auto& extension : extensions) {
        if (std::strcmp(
                extension.extensionName,
                extensionName) == 0) {
            return true;
        }
    }

    return false;
}

bool containsExtension(
        const std::vector<const char*>& extensions,
        const char* name) {
    return std::any_of(
            extensions.begin(),
            extensions.end(),
            [name](const char* existing) {
                return existing != nullptr &&
                       std::strcmp(existing, name) == 0;
            });
}

void addExtensionIfSupported(
        VkPhysicalDevice physicalDevice,
        std::vector<const char*>& extensions,
        const char* extensionName) {
    if (containsExtension(
            extensions,
            extensionName)) {
        return;
    }

    if (deviceSupportsExtension(
            physicalDevice,
            extensionName)) {
        extensions.push_back(extensionName);
    }
}

std::vector<const char*> bridgeDeviceExtensions(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* createInfo) {
    std::vector<const char*> extensions;

    if (createInfo != nullptr &&
        createInfo->enabledExtensionCount > 0 &&
        createInfo->ppEnabledExtensionNames != nullptr) {
        extensions.assign(
                createInfo->ppEnabledExtensionNames,
                createInfo->ppEnabledExtensionNames +
                        createInfo->enabledExtensionCount);
    }

    addExtensionIfSupported(
            physicalDevice,
            extensions,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);

    addExtensionIfSupported(
            physicalDevice,
            extensions,
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);

    addExtensionIfSupported(
            physicalDevice,
            extensions,
            VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);

    addExtensionIfSupported(
            physicalDevice,
            extensions,
            VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);

    addExtensionIfSupported(
            physicalDevice,
            extensions,
            VK_KHR_BIND_MEMORY_2_EXTENSION_NAME);

    addExtensionIfSupported(
            physicalDevice,
            extensions,
            VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME);

    return extensions;
}

void registerCreatedDevice(
        VkPhysicalDevice physicalDevice,
        VkDevice* device,
        VkResult result) {
    if (result == VK_SUCCESS &&
        device != nullptr &&
        *device != VK_NULL_HANDLE) {
        framegen::guest_bridge::registerDevice(
                physicalDevice,
                *device);
    }
}

void registerQueue(
        VkDevice device,
        uint32_t familyIndex,
        VkQueue* queue) {
    if (queue == nullptr ||
        *queue == VK_NULL_HANDLE) {
        return;
    }

    framegen::guest_bridge::registerQueue(
            device,
            *queue,
            familyIndex);
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
    if ((result != VK_SUCCESS &&
         result != VK_INCOMPLETE) ||
        images == nullptr ||
        imageCount == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(gRegistryMutex);
    auto it = gSwapchains.find(swapchain);

    if (it == gSwapchains.end()) {
        return;
    }

    it->second.images.assign(
            images,
            images + imageCount);
}

void populateSwapchainImages(
        VkDevice device,
        VkSwapchainKHR swapchain) {
    uint32_t count = 0;
    VkResult result =
            vkGetSwapchainImagesKHR(
                    device,
                    swapchain,
                    &count,
                    nullptr);

    if (result != VK_SUCCESS ||
        count == 0) {
        return;
    }

    std::vector<VkImage> images(count);

    result =
            vkGetSwapchainImagesKHR(
                    device,
                    swapchain,
                    &count,
                    images.data());

    recordImages(
            swapchain,
            count,
            images.data(),
            result);
}

void recordAcquire(
        VkSwapchainKHR swapchain,
        const uint32_t* imageIndex,
        VkResult result) {
    if ((result != VK_SUCCESS &&
         result != VK_SUBOPTIMAL_KHR) ||
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

CaptureInfo captureInfoForPresent(
        const VkPresentInfoKHR* presentInfo) {
    CaptureInfo info{};

    if (presentInfo == nullptr ||
        presentInfo->swapchainCount != 1 ||
        presentInfo->pSwapchains == nullptr) {
        return info;
    }

    const VkSwapchainKHR swapchain =
            presentInfo->pSwapchains[0];

    std::lock_guard<std::mutex> lock(gRegistryMutex);
    auto it = gSwapchains.find(swapchain);

    if (it == gSwapchains.end()) {
        return info;
    }

    const SwapchainRecord& record = it->second;

    if ((record.usage &
         VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
        record.images.empty()) {
        return info;
    }

    uint32_t imageIndex =
            record.lastAcquiredIndex;

    if (presentInfo->pImageIndices != nullptr) {
        imageIndex =
                presentInfo->pImageIndices[0];
    }

    if (imageIndex >= record.images.size()) {
        return info;
    }

    info.valid = true;
    info.device = record.device;
    info.swapchain = swapchain;
    info.image = record.images[imageIndex];
    info.format = record.format;
    info.extent = record.extent;
    return info;
}

void recordPresent(
        VkQueue queue,
        const VkPresentInfoKHR* presentInfo,
        VkResult result) {
    if ((result != VK_SUCCESS &&
         result != VK_SUBOPTIMAL_KHR) ||
        presentInfo == nullptr ||
        presentInfo->pSwapchains == nullptr ||
        presentInfo->swapchainCount == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(gRegistryMutex);

    for (uint32_t i = 0;
         i < presentInfo->swapchainCount;
         ++i) {
        const VkSwapchainKHR swapchain =
                presentInfo->pSwapchains[i];

        auto it =
                gSwapchains.find(swapchain);

        if (it == gSwapchains.end()) {
            continue;
        }

        SwapchainRecord& record =
                it->second;

        record.lastPresentQueue = queue;
        record.presentCount++;

        uint32_t imageIndex =
                record.lastAcquiredIndex;

        if (presentInfo->pImageIndices != nullptr) {
            imageIndex =
                    presentInfo->pImageIndices[i];
        }

        gLastPresented.valid = true;
        gLastPresented.format = record.format;
        gLastPresented.extent = record.extent;
        gLastPresented.usage = record.usage;
        gLastPresented.imageCount =
                static_cast<uint32_t>(
                        record.images.size());
        gLastPresented.imageIndex =
                imageIndex;
        gLastPresented.presentSerial =
                gPresentSerial.fetch_add(
                        1,
                        std::memory_order_relaxed) + 1;
    }
}

void recordDestroy(
        VkSwapchainKHR swapchain) {
    framegen::guest_bridge::unregisterSwapchain(
            swapchain);

    std::lock_guard<std::mutex> lock(gRegistryMutex);
    gSwapchains.erase(swapchain);
}

std::string registrySummary() {
    std::lock_guard<std::mutex> lock(gRegistryMutex);

    std::ostringstream out;
    out << "M2/M3 swapchain registry\n"
        << "Bridge enabled: "
        << (gGuestBridgeEnabled.load(
                    std::memory_order_relaxed)
                ? "yes"
                : "no")
        << "\n"
        << "Active swapchains: "
        << gSwapchains.size()
        << "\n";

    size_t ordinal = 0;

    for (const auto& entry : gSwapchains) {
        const SwapchainRecord& record =
                entry.second;

        out << "#" << (++ordinal)
            << " "
            << record.extent.width
            << "x"
            << record.extent.height
            << " format="
            << static_cast<int>(record.format)
            << " images="
            << record.images.size()
            << " usage=0x"
            << std::hex
            << record.usage
            << std::dec;

        if (record.hasAcquiredIndex) {
            out << " acquired="
                << record.lastAcquiredIndex;
        }

        out << " presents="
            << record.presentCount
            << "\n";
    }

    if (gLastPresented.valid) {
        out << "Last presented frame: "
            << gLastPresented.extent.width
            << "x"
            << gLastPresented.extent.height
            << " image="
            << gLastPresented.imageIndex
            << "/"
            << gLastPresented.imageCount
            << " format="
            << static_cast<int>(
                    gLastPresented.format)
            << " serial="
            << gLastPresented.presentSerial;
    } else {
        out << "Last presented frame: none yet";
    }

    return out.str();
}

VkResult createDeviceWithBridgeExtensions(
        PFN_vkCreateDevice real,
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* createInfo,
        const VkAllocationCallbacks* allocator,
        VkDevice* device) {
    if (real == nullptr ||
        createInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    std::vector<const char*> extensions =
            bridgeDeviceExtensions(
                    physicalDevice,
                    createInfo);

    VkDeviceCreateInfo modified =
            *createInfo;

    modified.enabledExtensionCount =
            static_cast<uint32_t>(
                    extensions.size());

    modified.ppEnabledExtensionNames =
            extensions.empty()
                    ? nullptr
                    : extensions.data();

    VkResult result =
            real(
                    physicalDevice,
                    &modified,
                    allocator,
                    device);

    registerCreatedDevice(
            physicalDevice,
            device,
            result);

    return result;
}

template <typename CreateFn>
VkResult createSwapchainWithBridgeUsage(
        CreateFn&& realCreate,
        VkDevice device,
        const VkSwapchainCreateInfoKHR* createInfo,
        const VkAllocationCallbacks* allocator,
        VkSwapchainKHR* swapchain) {
    if (createInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkSwapchainCreateInfoKHR modified =
            *createInfo;

    modified.imageUsage |=
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkResult result =
            realCreate(
                    device,
                    &modified,
                    allocator,
                    swapchain);

    const VkSwapchainCreateInfoKHR* usedInfo =
            &modified;

    if (result != VK_SUCCESS &&
        (createInfo->imageUsage &
         VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
        result =
                realCreate(
                        device,
                        createInfo,
                        allocator,
                        swapchain);

        usedInfo = createInfo;
    }

    if (swapchain != nullptr) {
        recordCreate(
                device,
                usedInfo,
                *swapchain,
                result);

        if (result == VK_SUCCESS) {
            populateSwapchainImages(
                    device,
                    *swapchain);
        }
    }

    return result;
}

VkResult presentWithOptionalBridge(
        PFN_vkQueuePresentKHR real,
        VkQueue queue,
        const VkPresentInfoKHR* presentInfo) {
    if (real == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (!gGuestBridgeEnabled.load(
            std::memory_order_acquire)) {
        VkResult result =
                real(
                        queue,
                        presentInfo);

        recordPresent(
                queue,
                presentInfo,
                result);

        return result;
    }

    const CaptureInfo info =
            captureInfoForPresent(
                    presentInfo);

    if (!info.valid) {
        VkResult result =
                real(
                        queue,
                        presentInfo);

        recordPresent(
                queue,
                presentInfo,
                result);

        return result;
    }

    const auto capture =
            framegen::guest_bridge::captureBeforePresent(
                    info.device,
                    info.swapchain,
                    queue,
                    info.image,
                    info.format,
                    info.extent,
                    presentInfo->waitSemaphoreCount,
                    presentInfo->pWaitSemaphores);

    if (!capture.active ||
        capture.presentWait == VK_NULL_HANDLE) {
        VkResult result =
                real(
                        queue,
                        presentInfo);

        recordPresent(
                queue,
                presentInfo,
                result);

        return result;
    }

    VkPresentInfoKHR modified =
            *presentInfo;

    modified.waitSemaphoreCount = 1;
    modified.pWaitSemaphores =
            &capture.presentWait;

    VkResult result =
            real(
                    queue,
                    &modified);

    recordPresent(
            queue,
            presentInfo,
            result);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL observedCreateDevice(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* createInfo,
        const VkAllocationCallbacks* allocator,
        VkDevice* device) {
    gCreateDeviceCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    return createDeviceWithBridgeExtensions(
            gRealCreateDevice.load(
                    std::memory_order_acquire),
            physicalDevice,
            createInfo,
            allocator,
            device);
}

VKAPI_ATTR void VKAPI_CALL observedDestroyDevice(
        VkDevice device,
        const VkAllocationCallbacks* allocator) {
    gDestroyDeviceCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    framegen::guest_bridge::unregisterDevice(
            device);

    PFN_vkDestroyDevice real =
            gRealDestroyDevice.load(
                    std::memory_order_acquire);

    if (real != nullptr) {
        real(
                device,
                allocator);
    }
}

VKAPI_ATTR void VKAPI_CALL observedGetDeviceQueue(
        VkDevice device,
        uint32_t queueFamilyIndex,
        uint32_t queueIndex,
        VkQueue* queue) {
    gGetDeviceQueueCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    PFN_vkGetDeviceQueue real =
            gRealGetDeviceQueue.load(
                    std::memory_order_acquire);

    if (real != nullptr) {
        real(
                device,
                queueFamilyIndex,
                queueIndex,
                queue);

        registerQueue(
                device,
                queueFamilyIndex,
                queue);
    }
}

VKAPI_ATTR void VKAPI_CALL observedGetDeviceQueue2(
        VkDevice device,
        const VkDeviceQueueInfo2* queueInfo,
        VkQueue* queue) {
    gGetDeviceQueueCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    PFN_vkGetDeviceQueue2 real =
            gRealGetDeviceQueue2.load(
                    std::memory_order_acquire);

    if (real != nullptr) {
        real(
                device,
                queueInfo,
                queue);

        if (queueInfo != nullptr) {
            registerQueue(
                    device,
                    queueInfo->queueFamilyIndex,
                    queue);
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL observedCreateSwapchain(
        VkDevice device,
        const VkSwapchainCreateInfoKHR* createInfo,
        const VkAllocationCallbacks* allocator,
        VkSwapchainKHR* swapchain) {
    gCreateSwapchainCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    PFN_vkCreateSwapchainKHR real =
            gRealCreateSwapchain.load(
                    std::memory_order_acquire);

    if (real == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    return createSwapchainWithBridgeUsage(
            [real](
                    VkDevice d,
                    const VkSwapchainCreateInfoKHR* info,
                    const VkAllocationCallbacks* alloc,
                    VkSwapchainKHR* out) {
                return real(
                        d,
                        info,
                        alloc,
                        out);
            },
            device,
            createInfo,
            allocator,
            swapchain);
}

VKAPI_ATTR void VKAPI_CALL observedDestroySwapchain(
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* allocator) {
    gDestroySwapchainCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    recordDestroy(swapchain);

    PFN_vkDestroySwapchainKHR real =
            gRealDestroySwapchain.load(
                    std::memory_order_acquire);

    if (real != nullptr) {
        real(
                device,
                swapchain,
                allocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL observedGetSwapchainImages(
        VkDevice device,
        VkSwapchainKHR swapchain,
        uint32_t* imageCount,
        VkImage* images) {
    gGetSwapchainImagesCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    PFN_vkGetSwapchainImagesKHR real =
            gRealGetSwapchainImages.load(
                    std::memory_order_acquire);

    if (real == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result =
            real(
                    device,
                    swapchain,
                    imageCount,
                    images);

    if (imageCount != nullptr &&
        images != nullptr) {
        recordImages(
                swapchain,
                *imageCount,
                images,
                result);
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
    gAcquireCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    PFN_vkAcquireNextImageKHR real =
            gRealAcquireNextImage.load(
                    std::memory_order_acquire);

    if (real == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result =
            real(
                    device,
                    swapchain,
                    timeout,
                    semaphore,
                    fence,
                    imageIndex);

    recordAcquire(
            swapchain,
            imageIndex,
            result);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL observedQueuePresent(
        VkQueue queue,
        const VkPresentInfoKHR* presentInfo) {
    gPresentCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    return presentWithOptionalBridge(
            gRealQueuePresent.load(
                    std::memory_order_acquire),
            queue,
            presentInfo);
}

PFN_vkVoidFunction proxyVkGetInstanceProcAddr(
        VkInstance instance,
        const char* name) {
    BYTEHOOK_STACK_SCOPE();

    gGetInstanceProcAddrCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    PFN_vkVoidFunction real =
            BYTEHOOK_CALL_PREV(
                    proxyVkGetInstanceProcAddr,
                    instance,
                    name);

    if (real == nullptr ||
        name == nullptr) {
        return real;
    }

    if (std::strcmp(
            name,
            "vkCreateDevice") == 0) {
        gRealCreateDevice.store(
                reinterpret_cast<PFN_vkCreateDevice>(
                        real),
                std::memory_order_release);

        return reinterpret_cast<PFN_vkVoidFunction>(
                observedCreateDevice);
    }

    return real;
}

PFN_vkVoidFunction proxyVkGetDeviceProcAddr(
        VkDevice device,
        const char* name) {
    BYTEHOOK_STACK_SCOPE();

    gGetDeviceProcAddrCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    PFN_vkVoidFunction real =
            BYTEHOOK_CALL_PREV(
                    proxyVkGetDeviceProcAddr,
                    device,
                    name);

    if (real == nullptr ||
        name == nullptr) {
        return real;
    }

    if (std::strcmp(
            name,
            "vkDestroyDevice") == 0) {
        gRealDestroyDevice.store(
                reinterpret_cast<PFN_vkDestroyDevice>(
                        real),
                std::memory_order_release);

        return reinterpret_cast<PFN_vkVoidFunction>(
                observedDestroyDevice);
    }

    if (std::strcmp(
            name,
            "vkGetDeviceQueue") == 0) {
        gRealGetDeviceQueue.store(
                reinterpret_cast<PFN_vkGetDeviceQueue>(
                        real),
                std::memory_order_release);

        return reinterpret_cast<PFN_vkVoidFunction>(
                observedGetDeviceQueue);
    }

    if (std::strcmp(
            name,
            "vkGetDeviceQueue2") == 0) {
        gRealGetDeviceQueue2.store(
                reinterpret_cast<PFN_vkGetDeviceQueue2>(
                        real),
                std::memory_order_release);

        return reinterpret_cast<PFN_vkVoidFunction>(
                observedGetDeviceQueue2);
    }

    if (std::strcmp(
            name,
            "vkCreateSwapchainKHR") == 0) {
        gRealCreateSwapchain.store(
                reinterpret_cast<PFN_vkCreateSwapchainKHR>(
                        real),
                std::memory_order_release);

        return reinterpret_cast<PFN_vkVoidFunction>(
                observedCreateSwapchain);
    }

    if (std::strcmp(
            name,
            "vkDestroySwapchainKHR") == 0) {
        gRealDestroySwapchain.store(
                reinterpret_cast<PFN_vkDestroySwapchainKHR>(
                        real),
                std::memory_order_release);

        return reinterpret_cast<PFN_vkVoidFunction>(
                observedDestroySwapchain);
    }

    if (std::strcmp(
            name,
            "vkGetSwapchainImagesKHR") == 0) {
        gRealGetSwapchainImages.store(
                reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
                        real),
                std::memory_order_release);

        return reinterpret_cast<PFN_vkVoidFunction>(
                observedGetSwapchainImages);
    }

    if (std::strcmp(
            name,
            "vkAcquireNextImageKHR") == 0) {
        gRealAcquireNextImage.store(
                reinterpret_cast<PFN_vkAcquireNextImageKHR>(
                        real),
                std::memory_order_release);

        return reinterpret_cast<PFN_vkVoidFunction>(
                observedAcquireNextImage);
    }

    if (std::strcmp(
            name,
            "vkQueuePresentKHR") == 0) {
        gRealQueuePresent.store(
                reinterpret_cast<PFN_vkQueuePresentKHR>(
                        real),
                std::memory_order_release);

        return reinterpret_cast<PFN_vkVoidFunction>(
                observedQueuePresent);
    }

    return real;
}

VKAPI_ATTR VkResult VKAPI_CALL proxyVkCreateDevice(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* createInfo,
        const VkAllocationCallbacks* allocator,
        VkDevice* device) {
    BYTEHOOK_STACK_SCOPE();

    gCreateDeviceCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    std::vector<const char*> extensions =
            bridgeDeviceExtensions(
                    physicalDevice,
                    createInfo);

    VkDeviceCreateInfo modified =
            *createInfo;

    modified.enabledExtensionCount =
            static_cast<uint32_t>(
                    extensions.size());

    modified.ppEnabledExtensionNames =
            extensions.empty()
                    ? nullptr
                    : extensions.data();

    VkResult result =
            BYTEHOOK_CALL_PREV(
                    proxyVkCreateDevice,
                    physicalDevice,
                    &modified,
                    allocator,
                    device);

    registerCreatedDevice(
            physicalDevice,
            device,
            result);

    return result;
}

VKAPI_ATTR void VKAPI_CALL proxyVkDestroyDevice(
        VkDevice device,
        const VkAllocationCallbacks* allocator) {
    BYTEHOOK_STACK_SCOPE();

    gDestroyDeviceCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    framegen::guest_bridge::unregisterDevice(
            device);

    BYTEHOOK_CALL_PREV(
            proxyVkDestroyDevice,
            device,
            allocator);
}

VKAPI_ATTR void VKAPI_CALL proxyVkGetDeviceQueue(
        VkDevice device,
        uint32_t queueFamilyIndex,
        uint32_t queueIndex,
        VkQueue* queue) {
    BYTEHOOK_STACK_SCOPE();

    gGetDeviceQueueCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    BYTEHOOK_CALL_PREV(
            proxyVkGetDeviceQueue,
            device,
            queueFamilyIndex,
            queueIndex,
            queue);

    registerQueue(
            device,
            queueFamilyIndex,
            queue);
}

VKAPI_ATTR void VKAPI_CALL proxyVkGetDeviceQueue2(
        VkDevice device,
        const VkDeviceQueueInfo2* queueInfo,
        VkQueue* queue) {
    BYTEHOOK_STACK_SCOPE();

    gGetDeviceQueueCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    BYTEHOOK_CALL_PREV(
            proxyVkGetDeviceQueue2,
            device,
            queueInfo,
            queue);

    if (queueInfo != nullptr) {
        registerQueue(
                device,
                queueInfo->queueFamilyIndex,
                queue);
    }
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

    return createSwapchainWithBridgeUsage(
            [](
                    VkDevice d,
                    const VkSwapchainCreateInfoKHR* info,
                    const VkAllocationCallbacks* alloc,
                    VkSwapchainKHR* out) {
                return BYTEHOOK_CALL_PREV(
                        proxyVkCreateSwapchainKHR,
                        d,
                        info,
                        alloc,
                        out);
            },
            device,
            createInfo,
            allocator,
            swapchain);
}

VKAPI_ATTR void VKAPI_CALL proxyVkDestroySwapchainKHR(
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* allocator) {
    BYTEHOOK_STACK_SCOPE();

    gDestroySwapchainCalls.fetch_add(
            1,
            std::memory_order_relaxed);

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

    gGetSwapchainImagesCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    VkResult result =
            BYTEHOOK_CALL_PREV(
                    proxyVkGetSwapchainImagesKHR,
                    device,
                    swapchain,
                    imageCount,
                    images);

    if (imageCount != nullptr &&
        images != nullptr) {
        recordImages(
                swapchain,
                *imageCount,
                images,
                result);
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

    gAcquireCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    VkResult result =
            BYTEHOOK_CALL_PREV(
                    proxyVkAcquireNextImageKHR,
                    device,
                    swapchain,
                    timeout,
                    semaphore,
                    fence,
                    imageIndex);

    recordAcquire(
            swapchain,
            imageIndex,
            result);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL proxyVkQueuePresentKHR(
        VkQueue queue,
        const VkPresentInfoKHR* presentInfo) {
    BYTEHOOK_STACK_SCOPE();

    gPresentCalls.fetch_add(
            1,
            std::memory_order_relaxed);

    if (!gGuestBridgeEnabled.load(
            std::memory_order_acquire)) {
        VkResult result =
                BYTEHOOK_CALL_PREV(
                        proxyVkQueuePresentKHR,
                        queue,
                        presentInfo);

        recordPresent(
                queue,
                presentInfo,
                result);

        return result;
    }

    const CaptureInfo info =
            captureInfoForPresent(
                    presentInfo);

    if (!info.valid) {
        VkResult result =
                BYTEHOOK_CALL_PREV(
                        proxyVkQueuePresentKHR,
                        queue,
                        presentInfo);

        recordPresent(
                queue,
                presentInfo,
                result);

        return result;
    }

    const auto capture =
            framegen::guest_bridge::captureBeforePresent(
                    info.device,
                    info.swapchain,
                    queue,
                    info.image,
                    info.format,
                    info.extent,
                    presentInfo->waitSemaphoreCount,
                    presentInfo->pWaitSemaphores);

    if (!capture.active ||
        capture.presentWait == VK_NULL_HANDLE) {
        VkResult result =
                BYTEHOOK_CALL_PREV(
                        proxyVkQueuePresentKHR,
                        queue,
                        presentInfo);

        recordPresent(
                queue,
                presentInfo,
                result);

        return result;
    }

    VkPresentInfoKHR modified =
            *presentInfo;

    modified.waitSemaphoreCount = 1;
    modified.pWaitSemaphores =
            &capture.presentWait;

    VkResult result =
            BYTEHOOK_CALL_PREV(
                    proxyVkQueuePresentKHR,
                    queue,
                    &modified);

    recordPresent(
            queue,
            presentInfo,
            result);

    return result;
}

std::string stats() {
    std::ostringstream out;

    out << "Guest Vulkan interception\n"
        << "vkGetInstanceProcAddr: "
        << gGetInstanceProcAddrCalls.load(
                std::memory_order_relaxed)
        << "\n"
        << "vkGetDeviceProcAddr: "
        << gGetDeviceProcAddrCalls.load(
                std::memory_order_relaxed)
        << "\n"
        << "vkCreateDevice: "
        << gCreateDeviceCalls.load(
                std::memory_order_relaxed)
        << "\n"
        << "vkGetDeviceQueue*: "
        << gGetDeviceQueueCalls.load(
                std::memory_order_relaxed)
        << "\n"
        << "vkCreateSwapchainKHR: "
        << gCreateSwapchainCalls.load(
                std::memory_order_relaxed)
        << "\n"
        << "vkGetSwapchainImagesKHR: "
        << gGetSwapchainImagesCalls.load(
                std::memory_order_relaxed)
        << "\n"
        << "vkAcquireNextImageKHR: "
        << gAcquireCalls.load(
                std::memory_order_relaxed)
        << "\n"
        << "vkQueuePresentKHR: "
        << gPresentCalls.load(
                std::memory_order_relaxed)
        << "\n"
        << "vkDestroySwapchainKHR: "
        << gDestroySwapchainCalls.load(
                std::memory_order_relaxed)
        << "\n"
        << "vkDestroyDevice: "
        << gDestroyDeviceCalls.load(
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
    if (gGipaStub != nullptr ||
        gGdpaStub != nullptr) {
        return toJString(
                env,
                "Guest Vulkan hooks already installed.\n" +
                        stats());
    }

    const int initResult =
            bytehook_init(
                    BYTEHOOK_MODE_AUTOMATIC,
                    false);

    if (initResult != BYTEHOOK_STATUS_CODE_OK) {
        std::ostringstream out;
        out << "ByteHook init failed. status="
            << initResult;
        return toJString(
                env,
                out.str());
    }

    gGipaStub =
            installGuestHook(
                    "vkGetInstanceProcAddr",
                    reinterpret_cast<void*>(
                            proxyVkGetInstanceProcAddr));

    gGdpaStub =
            installGuestHook(
                    "vkGetDeviceProcAddr",
                    reinterpret_cast<void*>(
                            proxyVkGetDeviceProcAddr));

    gCreateDeviceStub =
            installGuestHook(
                    "vkCreateDevice",
                    reinterpret_cast<void*>(
                            proxyVkCreateDevice));

    gDestroyDeviceStub =
            installGuestHook(
                    "vkDestroyDevice",
                    reinterpret_cast<void*>(
                            proxyVkDestroyDevice));

    gGetDeviceQueueStub =
            installGuestHook(
                    "vkGetDeviceQueue",
                    reinterpret_cast<void*>(
                            proxyVkGetDeviceQueue));

    gGetDeviceQueue2Stub =
            installGuestHook(
                    "vkGetDeviceQueue2",
                    reinterpret_cast<void*>(
                            proxyVkGetDeviceQueue2));

    gCreateSwapchainStub =
            installGuestHook(
                    "vkCreateSwapchainKHR",
                    reinterpret_cast<void*>(
                            proxyVkCreateSwapchainKHR));

    gDestroySwapchainStub =
            installGuestHook(
                    "vkDestroySwapchainKHR",
                    reinterpret_cast<void*>(
                            proxyVkDestroySwapchainKHR));

    gGetSwapchainImagesStub =
            installGuestHook(
                    "vkGetSwapchainImagesKHR",
                    reinterpret_cast<void*>(
                            proxyVkGetSwapchainImagesKHR));

    gAcquireStub =
            installGuestHook(
                    "vkAcquireNextImageKHR",
                    reinterpret_cast<void*>(
                            proxyVkAcquireNextImageKHR));

    gPresentStub =
            installGuestHook(
                    "vkQueuePresentKHR",
                    reinterpret_cast<void*>(
                            proxyVkQueuePresentKHR));

    if (gGipaStub == nullptr ||
        gGdpaStub == nullptr ||
        gCreateDeviceStub == nullptr ||
        gDestroyDeviceStub == nullptr ||
        gGetDeviceQueueStub == nullptr ||
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
            "M3 Vulkan hooks armed. "
            "Guest devices, queues, swapchains and GPU frame copies are tracked.");
}

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeGetVulkanHookStats(
        JNIEnv* env,
        jclass) {
    return toJString(
            env,
            stats());
}

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeGetSwapchainRegistry(
        JNIEnv* env,
        jclass) {
    return toJString(
            env,
            registrySummary());
}

extern "C"
JNIEXPORT void JNICALL
Java_dev_framegen_android_MainActivity_nativeSetGuestFrameBridgeEnabled(
        JNIEnv*,
        jclass,
        jboolean enabled) {
    gGuestBridgeEnabled.store(
            enabled == JNI_TRUE,
            std::memory_order_release);
}

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeGetGuestFrameBridgeStatus(
        JNIEnv* env,
        jclass) {
    return toJString(
            env,
            framegen::guest_bridge::status());
}
