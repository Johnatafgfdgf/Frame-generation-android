#include <jni.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string vkVersionToString(uint32_t version) {
    std::ostringstream out;
    out << VK_VERSION_MAJOR(version)
        << "."
        << VK_VERSION_MINOR(version)
        << "."
        << VK_VERSION_PATCH(version);
    return out.str();
}

jstring toJString(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

} // namespace

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeProbeVulkan(
        JNIEnv* env,
        jclass /* clazz */) {

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "FrameGenerationAndroid";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
    appInfo.pEngineName = "FrameGenRuntime";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 0, 1);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance);

    if (result != VK_SUCCESS) {
        std::ostringstream out;
        out << "Vulkan instance creation failed. VkResult=" << result;
        return toJString(env, out.str());
    }

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    if (result != VK_SUCCESS || deviceCount == 0) {
        vkDestroyInstance(instance, nullptr);
        return toJString(env, "Vulkan is present, but no physical GPU was exposed.");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    if (result != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        std::ostringstream out;
        out << "Could not enumerate Vulkan GPUs. VkResult=" << result;
        return toJString(env, out.str());
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(devices.front(), &props);

    std::ostringstream out;
    out << "Vulkan OK\n"
        << "GPU: " << props.deviceName << "\n"
        << "API: " << vkVersionToString(props.apiVersion) << "\n"
        << "Driver: " << props.driverVersion << "\n"
        << "Devices exposed: " << deviceCount;

    vkDestroyInstance(instance, nullptr);
    return toJString(env, out.str());
}
