#include <jni.h>
#include <vulkan/vulkan.h>

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

jstring toJString(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
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
        std::ostringstream out;
        out << "Guest vkCreateInstance failed: " << result;
        return toJString(env, out.str());
    }

    // This call is intentionally made through the imported Vulkan symbol.
    // ByteHook should redirect this PLT call to the host interceptor.
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
        std::ostringstream out;
        out << "Guest vkCreateDevice failed: " << result;
        return toJString(env, out.str());
    }

    // This should pass through proxyVkGetDeviceProcAddr.
    PFN_vkVoidFunction queuePresentFn =
            vkGetDeviceProcAddr(device, "vkQueuePresentKHR");
    PFN_vkVoidFunction createSwapchainFn =
            vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR");

    vkDeviceWaitIdle(device);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    std::ostringstream out;
    out << "Controlled guest Vulkan test finished.\n"
        << "GIPA pointer: " << (destroyInstanceFn != nullptr ? "yes" : "no") << "\n"
        << "GDPA vkQueuePresentKHR: " << (queuePresentFn != nullptr ? "yes" : "no") << "\n"
        << "GDPA vkCreateSwapchainKHR: " << (createSwapchainFn != nullptr ? "yes" : "no") << "\n"
        << "Swapchain extension supported: " << (hasSwapchain ? "yes" : "no");
    return toJString(env, out.str());
}
