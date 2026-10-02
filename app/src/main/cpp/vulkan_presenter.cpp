#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string vkError(const char* where, VkResult result) {
    std::ostringstream out;
    out << where << " failed. VkResult=" << result;
    return out.str();
}

jstring toJString(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

class VulkanPresenter {
public:
    ~VulkanPresenter() {
        shutdown();
    }

    std::string initialize(ANativeWindow* newWindow) {
        shutdown();

        window_ = newWindow;
        if (window_ == nullptr) {
            return "Surface has no ANativeWindow.";
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "FrameGenerationAndroid";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 0, 2);
        appInfo.pEngineName = "FrameGenCompositor";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 0, 2);
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

        VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            return fail(vkError("vkCreateInstance", result));
        }

        VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
        surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        surfaceInfo.window = window_;

        result = vkCreateAndroidSurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_);
        if (result != VK_SUCCESS) {
            return fail(vkError("vkCreateAndroidSurfaceKHR", result));
        }

        std::string selectError = selectPhysicalDevice();
        if (!selectError.empty()) {
            return fail(selectError);
        }

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        const char* deviceExtensions[] = {
                VK_KHR_SWAPCHAIN_EXTENSION_NAME
        };

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = deviceExtensions;

        result = vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_);
        if (result != VK_SUCCESS) {
            return fail(vkError("vkCreateDevice", result));
        }

        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

        std::string swapchainError = createSwapchain();
        if (!swapchainError.empty()) {
            return fail(swapchainError);
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;

        result = vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
        if (result != VK_SUCCESS) {
            return fail(vkError("vkCreateCommandPool", result));
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool_;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        result = vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer_);
        if (result != VK_SUCCESS) {
            return fail(vkError("vkAllocateCommandBuffers", result));
        }

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_);
        if (result != VK_SUCCESS) {
            return fail(vkError("vkCreateSemaphore(imageAvailable)", result));
        }

        result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinished_);
        if (result != VK_SUCCESS) {
            return fail(vkError("vkCreateSemaphore(renderFinished)", result));
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        result = vkCreateFence(device_, &fenceInfo, nullptr, &inFlight_);
        if (result != VK_SUCCESS) {
            return fail(vkError("vkCreateFence", result));
        }

        ready_ = true;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &props);

        std::ostringstream out;
        out << "Compositor ready\n"
            << "GPU: " << props.deviceName << "\n"
            << "Swapchain: " << extent_.width << "x" << extent_.height << "\n"
            << "Images: " << images_.size() << "\n"
            << "No MediaProjection";
        return out.str();
    }

    std::string draw(float phase) {
        if (!ready_) {
            return "Compositor is not initialized.";
        }

        VkResult result = vkWaitForFences(device_, 1, &inFlight_, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            return vkError("vkWaitForFences", result);
        }

        uint32_t imageIndex = 0;
        result = vkAcquireNextImageKHR(
                device_,
                swapchain_,
                UINT64_MAX,
                imageAvailable_,
                VK_NULL_HANDLE,
                &imageIndex
        );

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            return "Swapchain is out of date. Rotate/resize the Surface to recreate it.";
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return vkError("vkAcquireNextImageKHR", result);
        }

        result = vkResetFences(device_, 1, &inFlight_);
        if (result != VK_SUCCESS) {
            return vkError("vkResetFences", result);
        }

        result = vkResetCommandBuffer(commandBuffer_, 0);
        if (result != VK_SUCCESS) {
            return vkError("vkResetCommandBuffer", result);
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        result = vkBeginCommandBuffer(commandBuffer_, &beginInfo);
        if (result != VK_SUCCESS) {
            return vkError("vkBeginCommandBuffer", result);
        }

        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = imageInitialized_[imageIndex]
                ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                : VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = images_[imageIndex];
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.baseMipLevel = 0;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.baseArrayLayer = 0;
        toTransfer.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(
                commandBuffer_,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &toTransfer
        );

        const float wrapped = phase - static_cast<int>(phase);
        VkClearColorValue clearColor{};
        clearColor.float32[0] = 0.08f + 0.72f * wrapped;
        clearColor.float32[1] = 0.16f + 0.50f * (1.0f - wrapped);
        clearColor.float32[2] = 0.35f + 0.45f * (wrapped < 0.5f ? wrapped * 2.0f : (1.0f - wrapped) * 2.0f);
        clearColor.float32[3] = 1.0f;

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;

        vkCmdClearColorImage(
                commandBuffer_,
                images_[imageIndex],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &clearColor,
                1,
                &range
        );

        VkImageMemoryBarrier toPresent{};
        toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = images_[imageIndex];
        toPresent.subresourceRange = range;

        vkCmdPipelineBarrier(
                commandBuffer_,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &toPresent
        );

        result = vkEndCommandBuffer(commandBuffer_);
        if (result != VK_SUCCESS) {
            return vkError("vkEndCommandBuffer", result);
        }

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &imageAvailable_;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer_;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished_;

        result = vkQueueSubmit(queue_, 1, &submitInfo, inFlight_);
        if (result != VK_SUCCESS) {
            return vkError("vkQueueSubmit", result);
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished_;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(queue_, &presentInfo);
        imageInitialized_[imageIndex] = true;

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            return "Frame submitted, but swapchain became out of date.";
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return vkError("vkQueuePresentKHR", result);
        }

        std::ostringstream out;
        out << "Presented frame " << (++frameCounter_)
            << " directly through Vulkan (" << extent_.width << "x" << extent_.height << ")";
        return out.str();
    }

    void shutdown() {
        ready_ = false;

        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
        }

        if (device_ != VK_NULL_HANDLE && inFlight_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_, inFlight_, nullptr);
        }
        if (device_ != VK_NULL_HANDLE && renderFinished_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, renderFinished_, nullptr);
        }
        if (device_ != VK_NULL_HANDLE && imageAvailable_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, imageAvailable_, nullptr);
        }
        if (device_ != VK_NULL_HANDLE && commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, commandPool_, nullptr);
        }
        if (device_ != VK_NULL_HANDLE && swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        }
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE && surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
        }
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
        }

        window_ = nullptr;
        instance_ = VK_NULL_HANDLE;
        surface_ = VK_NULL_HANDLE;
        physicalDevice_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
        swapchain_ = VK_NULL_HANDLE;
        commandPool_ = VK_NULL_HANDLE;
        commandBuffer_ = VK_NULL_HANDLE;
        imageAvailable_ = VK_NULL_HANDLE;
        renderFinished_ = VK_NULL_HANDLE;
        inFlight_ = VK_NULL_HANDLE;
        images_.clear();
        imageInitialized_.clear();
        queueFamily_ = UINT32_MAX;
        frameCounter_ = 0;
    }

private:
    std::string fail(const std::string& message) {
        shutdown();
        return message;
    }

    std::string selectPhysicalDevice() {
        uint32_t deviceCount = 0;
        VkResult result = vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
        if (result != VK_SUCCESS || deviceCount == 0) {
            return "No Vulkan physical device was exposed.";
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        result = vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());
        if (result != VK_SUCCESS) {
            return vkError("vkEnumeratePhysicalDevices", result);
        }

        for (VkPhysicalDevice candidate : devices) {
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

            for (uint32_t i = 0; i < familyCount; ++i) {
                VkBool32 presentSupported = VK_FALSE;
                result = vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &presentSupported);
                if (result != VK_SUCCESS) {
                    continue;
                }

                const bool graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
                if (graphics && presentSupported == VK_TRUE) {
                    physicalDevice_ = candidate;
                    queueFamily_ = i;
                    return {};
                }
            }
        }

        return "No queue family supports both graphics and Android presentation.";
    }

    std::string createSwapchain() {
        VkSurfaceCapabilitiesKHR caps{};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                physicalDevice_, surface_, &caps);
        if (result != VK_SUCCESS) {
            return vkError("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
        }

        if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
            return "Swapchain images do not support TRANSFER_DST on this driver.";
        }

        uint32_t formatCount = 0;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
                physicalDevice_, surface_, &formatCount, nullptr);
        if (result != VK_SUCCESS || formatCount == 0) {
            return "No Vulkan surface formats are available.";
        }

        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(
                physicalDevice_, surface_, &formatCount, formats.data());

        VkSurfaceFormatKHR chosen = formats.front();
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_R8G8B8A8_UNORM ||
                format.format == VK_FORMAT_B8G8R8A8_UNORM) {
                chosen = format;
                break;
            }
        }
        surfaceFormat_ = chosen.format;

        if (caps.currentExtent.width != UINT32_MAX) {
            extent_ = caps.currentExtent;
        } else {
            const uint32_t width = static_cast<uint32_t>(
                    std::max(1, ANativeWindow_getWidth(window_)));
            const uint32_t height = static_cast<uint32_t>(
                    std::max(1, ANativeWindow_getHeight(window_)));

            extent_.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent_.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }

        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
            imageCount = caps.maxImageCount;
        }

        VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> alphaModes = {
                VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
        };
        for (auto mode : alphaModes) {
            if ((caps.supportedCompositeAlpha & mode) != 0) {
                compositeAlpha = mode;
                break;
            }
        }

        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface_;
        info.minImageCount = imageCount;
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = extent_;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = compositeAlpha;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = VK_NULL_HANDLE;

        result = vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_);
        if (result != VK_SUCCESS) {
            return vkError("vkCreateSwapchainKHR", result);
        }

        uint32_t actualImageCount = 0;
        result = vkGetSwapchainImagesKHR(device_, swapchain_, &actualImageCount, nullptr);
        if (result != VK_SUCCESS || actualImageCount == 0) {
            return "Swapchain was created but exposed no images.";
        }

        images_.resize(actualImageCount);
        result = vkGetSwapchainImagesKHR(
                device_, swapchain_, &actualImageCount, images_.data());
        if (result != VK_SUCCESS) {
            return vkError("vkGetSwapchainImagesKHR", result);
        }

        imageInitialized_.assign(images_.size(), false);
        return {};
    }

    ANativeWindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = UINT32_MAX;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat surfaceFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<bool> imageInitialized_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkSemaphore renderFinished_ = VK_NULL_HANDLE;
    VkFence inFlight_ = VK_NULL_HANDLE;

    bool ready_ = false;
    uint64_t frameCounter_ = 0;
};

VulkanPresenter gPresenter;

} // namespace

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeInitPresenter(
        JNIEnv* env,
        jclass,
        jobject surface) {
    if (surface == nullptr) {
        return toJString(env, "Java Surface was null.");
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    return toJString(env, gPresenter.initialize(window));
}

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeDrawFrame(
        JNIEnv* env,
        jclass,
        jfloat phase) {
    return toJString(env, gPresenter.draw(phase));
}

extern "C"
JNIEXPORT void JNICALL
Java_dev_framegen_android_MainActivity_nativeDestroyPresenter(
        JNIEnv*,
        jclass) {
    gPresenter.shutdown();
}
