#include "guest_frame_bridge.hpp"

#include <android/hardware_buffer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace framegen::guest_bridge {
namespace {

struct DeviceFunctions {
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProperties = nullptr;
};

struct DeviceState {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    DeviceFunctions fn{};
    std::unordered_map<VkQueue, uint32_t> queueFamilies;
};

struct SharedSlot {
    AHardwareBuffer* ahb = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkSemaphore copyFinished = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool initialized = false;
};

struct SwapchainBridge {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat sourceFormat = VK_FORMAT_UNDEFINED;
    VkFormat sharedFormat = VK_FORMAT_UNDEFINED;
    uint32_t queueFamily = UINT32_MAX;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::array<SharedSlot, 2> slots{};
    uint32_t writeSlot = 0;
    uint32_t previousSlot = UINT32_MAX;
    uint32_t currentSlot = UINT32_MAX;
    uint64_t serial = 0;
    bool ready = false;
    std::string disabledReason;
};

std::mutex gMutex;
std::unordered_map<VkDevice, DeviceState> gDevices;
std::unordered_map<VkSwapchainKHR, SwapchainBridge> gBridges;

uint64_t gCapturedFrames = 0;
uint64_t gBypassedFrames = 0;
std::string gLastMessage = "M3 bridge has not captured a frame yet.";

uint32_t ahbFormatForVk(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
            return AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
        default:
            return 0;
    }
}

bool isSuccess(VkResult result) {
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

void destroySlot(VkDevice device, SharedSlot& slot) {
    if (device != VK_NULL_HANDLE) {
        if (slot.fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, slot.fence, nullptr);
        }
        if (slot.copyFinished != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, slot.copyFinished, nullptr);
        }
        if (slot.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, slot.image, nullptr);
        }
        if (slot.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, slot.memory, nullptr);
        }
    }

    if (slot.ahb != nullptr) {
        AHardwareBuffer_release(slot.ahb);
    }

    slot = {};
}

void destroyBridge(SwapchainBridge& bridge) {
    if (bridge.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(bridge.device);
    }

    for (auto& slot : bridge.slots) {
        destroySlot(bridge.device, slot);
    }

    if (bridge.device != VK_NULL_HANDLE &&
        bridge.commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(
                bridge.device,
                bridge.commandPool,
                nullptr);
    }

    bridge = {};
}

bool formatSupports(
        VkPhysicalDevice physicalDevice,
        VkFormat format,
        VkFormatFeatureFlags feature) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(
            physicalDevice,
            format,
            &properties);

    return (properties.optimalTilingFeatures & feature) == feature;
}

bool canBlit(
        VkPhysicalDevice physicalDevice,
        VkFormat source,
        VkFormat destination) {
    return formatSupports(
                   physicalDevice,
                   source,
                   VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
           formatSupports(
                   physicalDevice,
                   destination,
                   VK_FORMAT_FEATURE_BLIT_DST_BIT);
}

bool createSlot(
        const DeviceState& deviceState,
        SwapchainBridge& bridge,
        SharedSlot& slot,
        std::string& error) {
    const uint32_t ahbFormat =
            ahbFormatForVk(bridge.sourceFormat);

    if (ahbFormat == 0) {
        error = "Guest swapchain format is not supported by the AHardwareBuffer bridge.";
        return false;
    }

    AHardwareBuffer_Desc description{};
    description.width = bridge.extent.width;
    description.height = bridge.extent.height;
    description.layers = 1;
    description.format = ahbFormat;
    description.usage =
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
            AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;

    if (AHardwareBuffer_allocate(
            &description,
            &slot.ahb) != 0 ||
        slot.ahb == nullptr) {
        error = "AHardwareBuffer_allocate failed.";
        return false;
    }

    VkAndroidHardwareBufferFormatPropertiesANDROID formatProperties{};
    formatProperties.sType =
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;

    VkAndroidHardwareBufferPropertiesANDROID ahbProperties{};
    ahbProperties.sType =
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    ahbProperties.pNext = &formatProperties;

    if (deviceState.fn.getAhbProperties == nullptr) {
        error = "vkGetAndroidHardwareBufferPropertiesANDROID is unavailable.";
        destroySlot(bridge.device, slot);
        return false;
    }

    VkResult result =
            deviceState.fn.getAhbProperties(
                    bridge.device,
                    slot.ahb,
                    &ahbProperties);

    if (result != VK_SUCCESS) {
        error = "Could not query AHardwareBuffer Vulkan properties.";
        destroySlot(bridge.device, slot);
        return false;
    }

    bridge.sharedFormat =
            formatProperties.format != VK_FORMAT_UNDEFINED
                    ? formatProperties.format
                    : (bridge.sourceFormat == VK_FORMAT_B8G8R8A8_UNORM ||
                       bridge.sourceFormat == VK_FORMAT_B8G8R8A8_SRGB)
                        ? VK_FORMAT_R8G8B8A8_UNORM
                        : bridge.sourceFormat;

    const VkExternalMemoryImageCreateInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes =
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };

    const VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &externalInfo,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = bridge.sharedFormat,
        .extent = {
            bridge.extent.width,
            bridge.extent.height,
            1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage =
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_STORAGE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    result = vkCreateImage(
            bridge.device,
            &imageInfo,
            nullptr,
            &slot.image);

    if (result != VK_SUCCESS ||
        slot.image == VK_NULL_HANDLE) {
        error = "vkCreateImage failed for AHardwareBuffer import.";
        destroySlot(bridge.device, slot);
        return false;
    }

    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(
            bridge.physicalDevice,
            &memoryProperties);

    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0;
         i < memoryProperties.memoryTypeCount;
         ++i) {
        if ((ahbProperties.memoryTypeBits & (1u << i)) != 0) {
            memoryTypeIndex = i;
            break;
        }
    }

    if (memoryTypeIndex == UINT32_MAX) {
        error = "No compatible Vulkan memory type for AHardwareBuffer.";
        destroySlot(bridge.device, slot);
        return false;
    }

    const VkMemoryDedicatedAllocateInfo dedicatedInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = slot.image,
        .buffer = VK_NULL_HANDLE,
    };

    const VkImportAndroidHardwareBufferInfoANDROID importInfo{
        .sType =
                VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .pNext = &dedicatedInfo,
        .buffer = slot.ahb,
    };

    const VkMemoryAllocateInfo allocationInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &importInfo,
        .allocationSize = ahbProperties.allocationSize,
        .memoryTypeIndex = memoryTypeIndex,
    };

    result = vkAllocateMemory(
            bridge.device,
            &allocationInfo,
            nullptr,
            &slot.memory);

    if (result != VK_SUCCESS ||
        slot.memory == VK_NULL_HANDLE) {
        error = "vkAllocateMemory failed for imported AHardwareBuffer.";
        destroySlot(bridge.device, slot);
        return false;
    }

    result = vkBindImageMemory(
            bridge.device,
            slot.image,
            slot.memory,
            0);

    if (result != VK_SUCCESS) {
        error = "vkBindImageMemory failed for imported AHardwareBuffer.";
        destroySlot(bridge.device, slot);
        return false;
    }

    const VkSemaphoreCreateInfo semaphoreInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    result = vkCreateSemaphore(
            bridge.device,
            &semaphoreInfo,
            nullptr,
            &slot.copyFinished);

    if (result != VK_SUCCESS) {
        error = "Could not create bridge semaphore.";
        destroySlot(bridge.device, slot);
        return false;
    }

    const VkFenceCreateInfo fenceInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    result = vkCreateFence(
            bridge.device,
            &fenceInfo,
            nullptr,
            &slot.fence);

    if (result != VK_SUCCESS) {
        error = "Could not create bridge fence.";
        destroySlot(bridge.device, slot);
        return false;
    }

    slot.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    slot.initialized = true;
    return true;
}

bool initializeBridge(
        SwapchainBridge& bridge,
        const DeviceState& deviceState,
        uint32_t queueFamily,
        std::string& error) {
    bridge.queueFamily = queueFamily;

    const VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamily,
    };

    VkResult result =
            vkCreateCommandPool(
                    bridge.device,
                    &poolInfo,
                    nullptr,
                    &bridge.commandPool);

    if (result != VK_SUCCESS) {
        error = "Could not create M3 command pool.";
        return false;
    }

    std::array<VkCommandBuffer, 2> buffers{};

    const VkCommandBufferAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = bridge.commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount =
                static_cast<uint32_t>(buffers.size()),
    };

    result = vkAllocateCommandBuffers(
            bridge.device,
            &allocateInfo,
            buffers.data());

    if (result != VK_SUCCESS) {
        error = "Could not allocate M3 command buffers.";
        return false;
    }

    for (size_t i = 0; i < bridge.slots.size(); ++i) {
        bridge.slots[i].commandBuffer = buffers[i];

        if (!createSlot(
                deviceState,
                bridge,
                bridge.slots[i],
                error)) {
            return false;
        }
    }

    bridge.ready = true;
    return true;
}

void recordCopyCommands(
        const DeviceState& deviceState,
        SwapchainBridge& bridge,
        SharedSlot& slot,
        VkImage sourceImage,
        VkCommandBuffer commandBuffer) {
    const VkImageSubresourceRange range{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    VkImageMemoryBarrier sourceToTransfer{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = sourceImage,
        .subresourceRange = range,
    };

    VkImageMemoryBarrier sharedToTransfer{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = slot.layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex =
                slot.layout == VK_IMAGE_LAYOUT_UNDEFINED
                        ? VK_QUEUE_FAMILY_IGNORED
                        : VK_QUEUE_FAMILY_EXTERNAL,
        .dstQueueFamilyIndex =
                slot.layout == VK_IMAGE_LAYOUT_UNDEFINED
                        ? VK_QUEUE_FAMILY_IGNORED
                        : bridge.queueFamily,
        .image = slot.image,
        .subresourceRange = range,
    };

    std::array<VkImageMemoryBarrier, 2> before{
        sourceToTransfer,
        sharedToTransfer,
    };

    vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            static_cast<uint32_t>(before.size()),
            before.data());

    const bool blitSupported =
            canBlit(
                    bridge.physicalDevice,
                    bridge.sourceFormat,
                    bridge.sharedFormat);

    if (bridge.sourceFormat == bridge.sharedFormat ||
        blitSupported) {
        if (blitSupported) {
            const VkImageBlit region{
                .srcSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .srcOffsets = {
                    {0, 0, 0},
                    {
                        static_cast<int32_t>(bridge.extent.width),
                        static_cast<int32_t>(bridge.extent.height),
                        1,
                    },
                },
                .dstSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .dstOffsets = {
                    {0, 0, 0},
                    {
                        static_cast<int32_t>(bridge.extent.width),
                        static_cast<int32_t>(bridge.extent.height),
                        1,
                    },
                },
            };

            vkCmdBlitImage(
                    commandBuffer,
                    sourceImage,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    slot.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1,
                    &region,
                    VK_FILTER_NEAREST);
        } else {
            const VkImageCopy region{
                .srcSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .srcOffset = {0, 0, 0},
                .dstSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .dstOffset = {0, 0, 0},
                .extent = {
                    bridge.extent.width,
                    bridge.extent.height,
                    1,
                },
            };

            vkCmdCopyImage(
                    commandBuffer,
                    sourceImage,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    slot.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1,
                    &region);
        }
    }

    VkImageMemoryBarrier sourceToPresent{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = sourceImage,
        .subresourceRange = range,
    };

    VkImageMemoryBarrier sharedToExternal{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = bridge.queueFamily,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .image = slot.image,
        .subresourceRange = range,
    };

    std::array<VkImageMemoryBarrier, 2> after{
        sourceToPresent,
        sharedToExternal,
    };

    vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            static_cast<uint32_t>(after.size()),
            after.data());

    slot.layout = VK_IMAGE_LAYOUT_GENERAL;
}

} // namespace

void registerDevice(
        VkPhysicalDevice physicalDevice,
        VkDevice device) {
    if (physicalDevice == VK_NULL_HANDLE ||
        device == VK_NULL_HANDLE) {
        return;
    }

    DeviceState state{};
    state.physicalDevice = physicalDevice;
    state.device = device;
    state.fn.getAhbProperties =
            reinterpret_cast<
                    PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
                    vkGetDeviceProcAddr(
                            device,
                            "vkGetAndroidHardwareBufferPropertiesANDROID"));

    std::lock_guard<std::mutex> lock(gMutex);
    gDevices[device] = std::move(state);
}

void unregisterDevice(VkDevice device) {
    std::lock_guard<std::mutex> lock(gMutex);

    for (auto it = gBridges.begin();
         it != gBridges.end();) {
        if (it->second.device == device) {
            destroyBridge(it->second);
            it = gBridges.erase(it);
        } else {
            ++it;
        }
    }

    gDevices.erase(device);
}

void registerQueue(
        VkDevice device,
        VkQueue queue,
        uint32_t familyIndex) {
    if (device == VK_NULL_HANDLE ||
        queue == VK_NULL_HANDLE) {
        return;
    }

    std::lock_guard<std::mutex> lock(gMutex);

    auto it = gDevices.find(device);
    if (it == gDevices.end()) {
        return;
    }

    it->second.queueFamilies[queue] =
            familyIndex;
}

void unregisterSwapchain(
        VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(gMutex);

    auto it = gBridges.find(swapchain);
    if (it == gBridges.end()) {
        return;
    }

    destroyBridge(it->second);
    gBridges.erase(it);
}

CaptureResult captureBeforePresent(
        VkDevice device,
        VkSwapchainKHR swapchain,
        VkQueue queue,
        VkImage sourceImage,
        VkFormat sourceFormat,
        VkExtent2D extent,
        uint32_t originalWaitCount,
        const VkSemaphore* originalWaitSemaphores) {
    std::lock_guard<std::mutex> lock(gMutex);

    CaptureResult result{};

    auto deviceIt = gDevices.find(device);
    if (deviceIt == gDevices.end()) {
        ++gBypassedFrames;
        result.message = "M3 bypass: guest VkDevice was not registered.";
        gLastMessage = result.message;
        return result;
    }

    auto queueIt =
            deviceIt->second.queueFamilies.find(queue);

    if (queueIt ==
        deviceIt->second.queueFamilies.end()) {
        ++gBypassedFrames;
        result.message = "M3 bypass: present queue family is unknown.";
        gLastMessage = result.message;
        return result;
    }

    auto bridgeIt = gBridges.find(swapchain);

    if (bridgeIt == gBridges.end()) {
        SwapchainBridge bridge{};
        bridge.device = device;
        bridge.physicalDevice =
                deviceIt->second.physicalDevice;
        bridge.swapchain = swapchain;
        bridge.extent = extent;
        bridge.sourceFormat = sourceFormat;

        bridgeIt =
                gBridges.emplace(
                        swapchain,
                        std::move(bridge)).first;
    }

    SwapchainBridge& bridge =
            bridgeIt->second;

    if (!bridge.ready) {
        std::string error;
        if (!initializeBridge(
                bridge,
                deviceIt->second,
                queueIt->second,
                error)) {
            bridge.disabledReason = error;
            ++gBypassedFrames;
            result.message = "M3 bypass: " + error;
            gLastMessage = result.message;
            return result;
        }
    }

    SharedSlot& slot =
            bridge.slots[bridge.writeSlot];

    VkResult vkResult =
            vkWaitForFences(
                    device,
                    1,
                    &slot.fence,
                    VK_TRUE,
                    UINT64_MAX);

    if (vkResult != VK_SUCCESS) {
        ++gBypassedFrames;
        result.message = "M3 bypass: bridge fence wait failed.";
        gLastMessage = result.message;
        return result;
    }

    vkResetFences(
            device,
            1,
            &slot.fence);

    vkResetCommandBuffer(
            slot.commandBuffer,
            0);

    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    vkResult =
            vkBeginCommandBuffer(
                    slot.commandBuffer,
                    &beginInfo);

    if (vkResult != VK_SUCCESS) {
        ++gBypassedFrames;
        result.message = "M3 bypass: vkBeginCommandBuffer failed.";
        gLastMessage = result.message;
        return result;
    }

    recordCopyCommands(
            deviceIt->second,
            bridge,
            slot,
            sourceImage,
            slot.commandBuffer);

    vkResult =
            vkEndCommandBuffer(
                    slot.commandBuffer);

    if (vkResult != VK_SUCCESS) {
        ++gBypassedFrames;
        result.message = "M3 bypass: vkEndCommandBuffer failed.";
        gLastMessage = result.message;
        return result;
    }

    std::vector<VkPipelineStageFlags> waitStages(
            originalWaitCount,
            VK_PIPELINE_STAGE_TRANSFER_BIT);

    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = originalWaitCount,
        .pWaitSemaphores = originalWaitSemaphores,
        .pWaitDstStageMask =
                originalWaitCount == 0
                        ? nullptr
                        : waitStages.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = &slot.commandBuffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &slot.copyFinished,
    };

    vkResult =
            vkQueueSubmit(
                    queue,
                    1,
                    &submitInfo,
                    slot.fence);

    if (vkResult != VK_SUCCESS) {
        ++gBypassedFrames;
        result.message = "M3 bypass: bridge vkQueueSubmit failed.";
        gLastMessage = result.message;
        return result;
    }

    bridge.previousSlot =
            bridge.currentSlot;
    bridge.currentSlot =
            bridge.writeSlot;
    bridge.writeSlot =
            (bridge.writeSlot + 1u) %
            static_cast<uint32_t>(bridge.slots.size());
    bridge.serial++;

    ++gCapturedFrames;

    result.active = true;
    result.presentWait = slot.copyFinished;
    result.serial = bridge.serial;

    std::ostringstream message;
    message << "M3 captured guest frame "
            << bridge.serial
            << " into AHardwareBuffer "
            << bridge.extent.width
            << "x" << bridge.extent.height
            << " srcFormat=" << static_cast<int>(bridge.sourceFormat)
            << " sharedFormat=" << static_cast<int>(bridge.sharedFormat);

    result.message = message.str();
    gLastMessage = result.message;
    return result;
}

std::string status() {
    std::lock_guard<std::mutex> lock(gMutex);

    std::ostringstream out;
    out << "M3 guest GPU bridge\n"
        << "Registered devices: "
        << gDevices.size() << "\n"
        << "Active swapchain bridges: "
        << gBridges.size() << "\n"
        << "Captured frames: "
        << gCapturedFrames << "\n"
        << "Bypassed frames: "
        << gBypassedFrames << "\n"
        << gLastMessage;

    return out.str();
}

bool getLatestPair(
        VkSwapchainKHR swapchain,
        AHardwareBuffer** previous,
        AHardwareBuffer** current,
        VkExtent2D* extent,
        VkFormat* format,
        uint64_t* serial) {
    std::lock_guard<std::mutex> lock(gMutex);

    auto it = gBridges.find(swapchain);
    if (it == gBridges.end()) {
        return false;
    }

    SwapchainBridge& bridge = it->second;

    if (!bridge.ready ||
        bridge.previousSlot == UINT32_MAX ||
        bridge.currentSlot == UINT32_MAX) {
        return false;
    }

    if (previous != nullptr) {
        *previous =
                bridge.slots[bridge.previousSlot].ahb;
    }

    if (current != nullptr) {
        *current =
                bridge.slots[bridge.currentSlot].ahb;
    }

    if (extent != nullptr) {
        *extent = bridge.extent;
    }

    if (format != nullptr) {
        *format = bridge.sharedFormat;
    }

    if (serial != nullptr) {
        *serial = bridge.serial;
    }

    return true;
}

} // namespace framegen::guest_bridge
