#include <jni.h>
#include <android/hardware_buffer.h>
#include <vulkan/vulkan.h>

#include <sstream>
#include <string>
#include <vector>

namespace {

jstring toJString(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

bool hasDeviceExtension(
        VkPhysicalDevice device,
        const char* extensionName) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(
            device,
            nullptr,
            &count,
            nullptr) != VK_SUCCESS) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(
            device,
            nullptr,
            &count,
            extensions.data()) != VK_SUCCESS) {
        return false;
    }

    for (const auto& extension : extensions) {
        if (std::string(extension.extensionName) == extensionName) {
            return true;
        }
    }

    return false;
}

} // namespace

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeProbeFrameBridge(
        JNIEnv* env,
        jclass) {
    AHardwareBuffer_Desc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage =
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
            AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;

    AHardwareBuffer* buffer = nullptr;
    const int ahbResult = AHardwareBuffer_allocate(&desc, &buffer);

    AHardwareBuffer_Desc actual{};
    bool ahbOk = false;
    if (ahbResult == 0 && buffer != nullptr) {
        AHardwareBuffer_describe(buffer, &actual);
        ahbOk = actual.width == 64 &&
                actual.height == 64 &&
                actual.layers == 1;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "FrameBridgeProbe";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
    appInfo.pEngineName = "FrameBridge";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 2, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult vkResult = vkCreateInstance(
            &instanceInfo,
            nullptr,
            &instance);

    bool externalAhb = false;
    bool externalMemory = false;
    bool samplerYcbcr = false;
    bool foreignQueue = false;
    std::string gpuName = "unavailable";

    if (vkResult == VK_SUCCESS) {
        uint32_t deviceCount = 0;
        if (vkEnumeratePhysicalDevices(
                instance,
                &deviceCount,
                nullptr) == VK_SUCCESS &&
            deviceCount > 0) {
            std::vector<VkPhysicalDevice> devices(deviceCount);
            if (vkEnumeratePhysicalDevices(
                    instance,
                    &deviceCount,
                    devices.data()) == VK_SUCCESS) {
                VkPhysicalDevice device = devices.front();

                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(device, &props);
                gpuName = props.deviceName;

                externalAhb = hasDeviceExtension(
                        device,
                        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
                externalMemory = hasDeviceExtension(
                        device,
                        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
                samplerYcbcr = hasDeviceExtension(
                        device,
                        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME);
                foreignQueue = hasDeviceExtension(
                        device,
                        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
            }
        }

        vkDestroyInstance(instance, nullptr);
    }

    if (buffer != nullptr) {
        AHardwareBuffer_release(buffer);
    }

    std::ostringstream out;
    out << "M2 GPU frame bridge probe\n"
        << "GPU: " << gpuName << "\n"
        << "AHardwareBuffer GPU allocation: "
        << (ahbOk ? "yes" : "no")
        << " (result=" << ahbResult << ")\n"
        << "VK_ANDROID_external_memory_android_hardware_buffer: "
        << (externalAhb ? "yes" : "no") << "\n"
        << "VK_KHR_external_memory: "
        << (externalMemory ? "yes" : "no") << "\n"
        << "VK_KHR_sampler_ycbcr_conversion: "
        << (samplerYcbcr ? "yes" : "no") << "\n"
        << "VK_EXT_queue_family_foreign: "
        << (foreignQueue ? "yes" : "no") << "\n";

    if (ahbOk && externalAhb) {
        out << "Bridge candidate: compatible. "
            << "Next step is importing the same AHardwareBuffer "
            << "as VkImage on both Vulkan devices.";
    } else {
        out << "Bridge candidate: incomplete on this driver.";
    }

    return toJString(env, out.str());
}
