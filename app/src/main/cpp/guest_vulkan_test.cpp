#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

jstring toJString(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

std::string vkError(const char* where, VkResult result) {
    std::ostringstream out;
    out << where << " failed. VkResult=" << result;
    return out.str();
}

bool supportsSwapchain(VkPhysicalDevice physicalDevice) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(
            physicalDevice, nullptr, &count, extensions.data()) != VK_SUCCESS) {
        return false;
    }

    for (const auto& ext : extensions) {
        if (std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            return true;
        }
    }
    return false;
}

VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(
        VkCompositeAlphaFlagsKHR supported) {
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> modes = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
    };

    for (auto mode : modes) {
        if ((supported & mode) != 0) {
            return mode;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

} // namespace

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeRunGuestVulkanTest(
        JNIEnv* env,
        jclass) {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "FrameGenGuestTest";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "GuestTest";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        return toJString(env, vkError("Guest vkCreateInstance", result));
    }

    PFN_vkVoidFunction destroyInstanceFn =
            vkGetInstanceProcAddr(instance, "vkDestroyInstance");

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS || deviceCount == 0) {
        vkDestroyInstance(instance, nullptr);
        return toJString(env, "Guest could not enumerate a Vulkan GPU.");
    }

    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());
    VkPhysicalDevice physicalDevice = physicalDevices.front();

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
            physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
            physicalDevice, &queueFamilyCount, queueFamilies.data());

    uint32_t queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            queueFamily = i;
            break;
        }
    }

    if (queueFamily == UINT32_MAX) {
        vkDestroyInstance(instance, nullptr);
        return toJString(env, "Guest found no graphics queue family.");
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const bool hasSwapchain = supportsSwapchain(physicalDevice);
    const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    if (hasSwapchain) {
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = &swapchainExtension;
    }

    VkDevice device = VK_NULL_HANDLE;
    result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device);
    if (result != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return toJString(env, vkError("Guest vkCreateDevice", result));
    }

    PFN_vkVoidFunction queuePresentFn =
            vkGetDeviceProcAddr(device, "vkQueuePresentKHR");
    PFN_vkVoidFunction createSwapchainFn =
            vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR");

    vkDeviceWaitIdle(device);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    std::ostringstream out;
    out << "Controlled guest Vulkan lookup test finished.\n"
        << "GIPA pointer: " << (destroyInstanceFn != nullptr ? "yes" : "no") << "\n"
        << "GDPA vkQueuePresentKHR: " << (queuePresentFn != nullptr ? "yes" : "no") << "\n"
        << "GDPA vkCreateSwapchainKHR: " << (createSwapchainFn != nullptr ? "yes" : "no") << "\n"
        << "Swapchain extension supported: " << (hasSwapchain ? "yes" : "no");
    return toJString(env, out.str());
}

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeRunGuestSurfaceTest(
        JNIEnv* env,
        jclass,
        jobject javaSurface,
        jfloat phase) {
    if (javaSurface == nullptr) {
        return toJString(env, "Guest Surface is null.");
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, javaSurface);
    if (window == nullptr) {
        return toJString(env, "Could not obtain guest ANativeWindow.");
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;

    auto cleanup = [&]() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
        }
        if (device != VK_NULL_HANDLE && renderFinished != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, renderFinished, nullptr);
        }
        if (device != VK_NULL_HANDLE && imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, imageAvailable, nullptr);
        }
        if (device != VK_NULL_HANDLE && commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, commandPool, nullptr);
        }
        if (device != VK_NULL_HANDLE && swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        }
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
        ANativeWindow_release(window);
    };

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "FrameGenGuestSurface";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 1);
    appInfo.pEngineName = "GuestSurfaceTest";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 1);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    const char* instanceExtensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    };

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;

    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        cleanup();
        return toJString(env, vkError("Guest surface vkCreateInstance", result));
    }

    VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.window = window;

    result = vkCreateAndroidSurfaceKHR(instance, &surfaceInfo, nullptr, &surface);
    if (result != VK_SUCCESS) {
        cleanup();
        return toJString(env, vkError("Guest vkCreateAndroidSurfaceKHR", result));
    }

    uint32_t physicalCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr);
    if (result != VK_SUCCESS || physicalCount == 0) {
        cleanup();
        return toJString(env, "Guest surface test found no Vulkan GPU.");
    }

    std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
    vkEnumeratePhysicalDevices(
            instance, &physicalCount, physicalDevices.data());

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t queueFamily = UINT32_MAX;

    for (VkPhysicalDevice candidate : physicalDevices) {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
                candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(
                candidate, &familyCount, families.data());

        for (uint32_t i = 0; i < familyCount; ++i) {
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(
                    candidate, i, surface, &presentSupport);

            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                presentSupport == VK_TRUE) {
                physicalDevice = candidate;
                queueFamily = i;
                break;
            }
        }

        if (physicalDevice != VK_NULL_HANDLE) {
            break;
        }
    }

    if (physicalDevice == VK_NULL_HANDLE || !supportsSwapchain(physicalDevice)) {
        cleanup();
        return toJString(
                env,
                "Guest found no graphics+present queue with VK_KHR_swapchain.");
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;

    result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device);
    if (result != VK_SUCCESS) {
        cleanup();
        return toJString(env, vkError("Guest surface vkCreateDevice", result));
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VkSurfaceCapabilitiesKHR caps{};
    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physicalDevice, surface, &caps);
    if (result != VK_SUCCESS) {
        cleanup();
        return toJString(
                env,
                vkError("Guest vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result));
    }

    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
        cleanup();
        return toJString(
                env,
                "Guest swapchain does not support TRANSFER_DST.");
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice, surface, &formatCount, nullptr);
    if (formatCount == 0) {
        cleanup();
        return toJString(env, "Guest Surface exposes no formats.");
    }

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice, surface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosenFormat = formats.front();
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM ||
            format.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosenFormat = format;
            break;
        }
    }

    VkExtent2D extent{};
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        const uint32_t width = static_cast<uint32_t>(
                std::max(1, ANativeWindow_getWidth(window)));
        const uint32_t height = static_cast<uint32_t>(
                std::max(1, ANativeWindow_getHeight(window)));
        extent.width = std::clamp(
                width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(
                height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = chosenFormat.format;
    swapchainInfo.imageColorSpace = chosenFormat.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = caps.currentTransform;
    swapchainInfo.compositeAlpha =
            chooseCompositeAlpha(caps.supportedCompositeAlpha);
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;

    // Direct call intentionally passes through ByteHook's guest-only hook.
    result = vkCreateSwapchainKHR(
            device, &swapchainInfo, nullptr, &swapchain);
    if (result != VK_SUCCESS) {
        cleanup();
        return toJString(env, vkError("Guest vkCreateSwapchainKHR", result));
    }

    uint32_t actualImageCount = 0;
    result = vkGetSwapchainImagesKHR(
            device, swapchain, &actualImageCount, nullptr);
    if (result != VK_SUCCESS || actualImageCount == 0) {
        cleanup();
        return toJString(env, "Guest swapchain exposed no images.");
    }

    std::vector<VkImage> images(actualImageCount);
    vkGetSwapchainImagesKHR(
            device, swapchain, &actualImageCount, images.data());

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;

    result = vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
    if (result != VK_SUCCESS) {
        cleanup();
        return toJString(env, vkError("Guest vkCreateCommandPool", result));
    }

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    result = vkAllocateCommandBuffers(
            device, &allocInfo, &commandBuffer);
    if (result != VK_SUCCESS) {
        cleanup();
        return toJString(
                env,
                vkError("Guest vkAllocateCommandBuffers", result));
    }

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    result = vkCreateSemaphore(
            device, &semaphoreInfo, nullptr, &imageAvailable);
    if (result != VK_SUCCESS) {
        cleanup();
        return toJString(env, vkError("Guest image semaphore", result));
    }

    result = vkCreateSemaphore(
            device, &semaphoreInfo, nullptr, &renderFinished);
    if (result != VK_SUCCESS) {
        cleanup();
        return toJString(env, vkError("Guest render semaphore", result));
    }

    uint32_t imageIndex = 0;
    result = vkAcquireNextImageKHR(
            device,
            swapchain,
            UINT64_MAX,
            imageAvailable,
            VK_NULL_HANDLE,
            &imageIndex);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        cleanup();
        return toJString(
                env,
                vkError("Guest vkAcquireNextImageKHR", result));
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = images[imageIndex];
    toTransfer.subresourceRange = range;

    vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &toTransfer);

    const float wrapped = phase - static_cast<int>(phase);
    VkClearColorValue clear{};
    clear.float32[0] = 0.65f;
    clear.float32[1] = 0.08f + wrapped * 0.75f;
    clear.float32[2] = 0.18f + (1.0f - wrapped) * 0.55f;
    clear.float32[3] = 1.0f;

    vkCmdClearColorImage(
            commandBuffer,
            images[imageIndex],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &clear,
            1,
            &range);

    VkImageMemoryBarrier toPresent{};
    toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = images[imageIndex];
    toPresent.subresourceRange = range;

    vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &toPresent);

    result = vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) {
        cleanup();
        return toJString(env, vkError("Guest vkEndCommandBuffer", result));
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished;

    result = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        cleanup();
        return toJString(env, vkError("Guest vkQueueSubmit", result));
    }

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;

    // This is the exact presentation boundary M2 will use.
    result = vkQueuePresentKHR(queue, &present);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        cleanup();
        return toJString(env, vkError("Guest vkQueuePresentKHR", result));
    }

    vkQueueWaitIdle(queue);

    std::ostringstream out;
    out << "M1B guest frame presented\n"
        << "Surface: " << extent.width << "x" << extent.height << "\n"
        << "Swapchain images: " << actualImageCount << "\n"
        << "MediaProjection: not used";

    cleanup();
    return toJString(env, out.str());
}
