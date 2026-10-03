#include <android/log.h>
#include <vulkan/vulkan.h>
#include "lsfg_vkd.h"

LsfgVkDispatch vkd;

namespace {
bool g_ready = false;

template <typename T>
T load_i(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, const char* name) {
    return reinterpret_cast<T>(gipa(inst, name));
}

template <typename T>
T load_d(PFN_vkGetDeviceProcAddr gdpa, VkDevice dev, const char* name) {
    return reinterpret_cast<T>(gdpa(dev, name));
}
}

bool lsfgVkdInit(const VkTable&) {
    return g_ready;
}

bool lsfgVkdReady() {
    return g_ready;
}

extern "C" bool zlsfg_fill_dispatch(
    PFN_vkGetInstanceProcAddr gipa,
    VkInstance instance,
    PFN_vkGetDeviceProcAddr gdpa,
    VkDevice device
) {
    bool ok = true;

#define LOAD_D(fn) \
    do { \
        vkd.fn = load_d<PFN_vk##fn>(gdpa, device, "vk" #fn); \
        if (!vkd.fn) { \
            __android_log_print(ANDROID_LOG_ERROR, "ZalithLSFG", "missing vk%s", #fn); \
            ok = false; \
        } \
    } while (0)

    LOAD_D(AllocateDescriptorSets);
    LOAD_D(AllocateMemory);
    LOAD_D(BindBufferMemory);
    LOAD_D(BindImageMemory);
    LOAD_D(CmdBindDescriptorSets);
    LOAD_D(CmdBindPipeline);
    LOAD_D(CmdCopyImage);
    LOAD_D(CmdDispatch);
    LOAD_D(CmdPipelineBarrier);
    LOAD_D(CreateBuffer);
    LOAD_D(CreateComputePipelines);
    LOAD_D(CreateDescriptorPool);
    LOAD_D(CreateDescriptorSetLayout);
    LOAD_D(CreateImage);
    LOAD_D(CreateImageView);
    LOAD_D(CreatePipelineLayout);
    LOAD_D(CreateSampler);
    LOAD_D(CreateShaderModule);
    LOAD_D(DestroyBuffer);
    LOAD_D(DestroyDescriptorPool);
    LOAD_D(DestroyDescriptorSetLayout);
    LOAD_D(DestroyImage);
    LOAD_D(DestroyImageView);
    LOAD_D(DestroyPipeline);
    LOAD_D(DestroyPipelineLayout);
    LOAD_D(DestroySampler);
    LOAD_D(DestroyShaderModule);
    LOAD_D(FreeMemory);
    LOAD_D(GetBufferMemoryRequirements);
    LOAD_D(GetImageMemoryRequirements);
    LOAD_D(MapMemory);
    LOAD_D(UnmapMemory);
    LOAD_D(UpdateDescriptorSets);
#undef LOAD_D

    vkd.GetPhysicalDeviceMemoryProperties =
        load_i<PFN_vkGetPhysicalDeviceMemoryProperties>(
            gipa, instance, "vkGetPhysicalDeviceMemoryProperties"
        );
    if (!vkd.GetPhysicalDeviceMemoryProperties) {
        __android_log_print(ANDROID_LOG_ERROR, "ZalithLSFG",
                            "missing vkGetPhysicalDeviceMemoryProperties");
        ok = false;
    }

    g_ready = ok;
    return ok;
}
