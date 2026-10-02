#pragma once

#include <android/hardware_buffer.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace framegen::guest_bridge {

struct CaptureResult {
    bool active = false;
    VkSemaphore presentWait = VK_NULL_HANDLE;
    uint64_t serial = 0;
    std::string message;
};

void registerDevice(VkPhysicalDevice physicalDevice, VkDevice device);
void unregisterDevice(VkDevice device);

void registerQueue(
        VkDevice device,
        VkQueue queue,
        uint32_t familyIndex);

void unregisterSwapchain(VkSwapchainKHR swapchain);

CaptureResult captureBeforePresent(
        VkDevice device,
        VkSwapchainKHR swapchain,
        VkQueue queue,
        VkImage sourceImage,
        VkFormat sourceFormat,
        VkExtent2D extent,
        uint32_t originalWaitCount,
        const VkSemaphore* originalWaitSemaphores);

std::string status();

// The returned AHardwareBuffer pointers remain owned by this module.
// Callers that keep them past the call must AHardwareBuffer_acquire().
bool getLatestPair(
        VkSwapchainKHR swapchain,
        AHardwareBuffer** previous,
        AHardwareBuffer** current,
        VkExtent2D* extent,
        VkFormat* format,
        uint64_t* serial);

} // namespace framegen::guest_bridge
