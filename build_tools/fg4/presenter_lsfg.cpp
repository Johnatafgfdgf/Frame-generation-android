#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include "lsfg_engine.h"
#include "lsfg_dll.h"
#include "vk_dispatch.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <memory>
#include <sys/stat.h>
#include <thread>
#include <vector>

#define ZFG_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ZalithLSFG", __VA_ARGS__)
#define ZFG_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ZalithLSFG", __VA_ARGS__)

extern "C" bool zlsfg_fill_dispatch(
    PFN_vkGetInstanceProcAddr gipa,
    VkInstance instance,
    PFN_vkGetDeviceProcAddr gdpa,
    VkDevice device
);

namespace {

struct SubmitConfig {
    int multiplier = 2;
    int perfPreset = 1;
    int targetFps = 60;
    bool lowLatency = true;
    bool sceneProtection = true;
};

template <typename T>
T load_global(PFN_vkGetInstanceProcAddr gipa, const char* name) {
    return reinterpret_cast<T>(gipa(VK_NULL_HANDLE, name));
}

template <typename T>
T load_instance(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, const char* name) {
    return reinterpret_cast<T>(gipa(inst, name));
}

template <typename T>
T load_device(PFN_vkGetDeviceProcAddr gdpa, VkDevice dev, const char* name) {
    return reinterpret_cast<T>(gdpa(dev, name));
}

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    bool valid = false;
};

class Presenter {
public:
    Presenter() = default;
    ~Presenter() { shutdown(); }

    bool submit(ANativeWindow* window, const void* pixels, int width, int height, int stride,
                const SubmitConfig& cfg) {
        if (!window || !pixels || width <= 0 || height <= 0 || stride < width) return false;
        if (fatal_.load(std::memory_order_acquire)) return false;

        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        std::vector<uint8_t> packed(bytes);
        const auto* src = static_cast<const uint8_t*>(pixels);
        for (int y = 0; y < height; ++y) {
            std::memcpy(
                packed.data() + static_cast<size_t>(y) * width * 4u,
                src + static_cast<size_t>(y) * stride * 4u,
                static_cast<size_t>(width) * 4u
            );
        }

        ANativeWindow_acquire(window);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pendingWindow_) ANativeWindow_release(pendingWindow_);
            pendingWindow_ = window;
            pending_.swap(packed);
            pendingW_ = width;
            pendingH_ = height;
            pendingCfg_ = cfg;
            ++pendingSequence_;
            realSubmitted_.fetch_add(1, std::memory_order_relaxed);
            if (!workerStarted_) {
                workerStarted_ = true;
                worker_ = std::thread(&Presenter::workerLoop, this);
            }
        }
        cv_.notify_one();
        return true;
    }

    void releaseSurfaceSync() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!workerStarted_) return;
        const uint64_t request = ++releaseRequest_;
        cv_.notify_one();
        releaseCv_.wait(lock, [&] {
            return releaseAck_ >= request || !workerStarted_ || fatal_.load(std::memory_order_acquire);
        });
    }

    void stats(uint64_t out[4]) const {
        out[0] = ready_.load(std::memory_order_acquire) ? 1u :
                 (fatal_.load(std::memory_order_acquire) ? 2u : 0u);
        out[1] = realSubmitted_.load(std::memory_order_relaxed);
        out[2] = generated_.load(std::memory_order_relaxed);
        out[3] = presented_.load(std::memory_order_relaxed);
    }

private:
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            cv_.notify_one();
        }
        if (worker_.joinable()) worker_.join();
        workerStarted_ = false;
    }

    void workerLoop() {
        uint64_t seen = 0;
        for (;;) {
            std::vector<uint8_t> frame;
            ANativeWindow* frameWindow = nullptr;
            int w = 0, h = 0;
            SubmitConfig cfg{};
            uint64_t releaseToAck = 0;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] {
                    return stop_ || pendingSequence_ != seen || releaseRequest_ > releaseAck_;
                });
                if (stop_) break;

                if (releaseRequest_ > releaseAck_) {
                    releaseToAck = releaseRequest_;
                }

                if (pendingSequence_ != seen) {
                    seen = pendingSequence_;
                    frame.swap(pending_);
                    w = pendingW_;
                    h = pendingH_;
                    cfg = pendingCfg_;
                    frameWindow = pendingWindow_;
                    pendingWindow_ = nullptr;
                }
            }

            if (releaseToAck) {
                destroySurfaceResources();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    releaseAck_ = std::max(releaseAck_, releaseToAck);
                }
                releaseCv_.notify_all();
            }

            if (!frameWindow || frame.empty()) continue;

            bool ok = ensureReady(frameWindow, w, h);
            if (ok) ok = processFrame(frame, w, h, cfg);
            ANativeWindow_release(frameWindow);

            if (!ok) {
                ZFG_LOGE("LSFG presenter failed; switching to launcher fallback");
                fatal_.store(true, std::memory_order_release);
                ready_.store(false, std::memory_order_release);
                destroyAll();
                std::lock_guard<std::mutex> lock(mutex_);
                releaseAck_ = releaseRequest_;
                releaseCv_.notify_all();
            }
        }

        destroyAll();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pendingWindow_) {
                ANativeWindow_release(pendingWindow_);
                pendingWindow_ = nullptr;
            }
            workerStarted_ = false;
            releaseAck_ = releaseRequest_;
        }
        releaseCv_.notify_all();
    }

    bool ensureReady(ANativeWindow* window, int w, int h) {
        if (!vkReady_ && !initVulkan()) return false;

        if (window_ != window || surfaceW_ != w || surfaceH_ != h || swapchain_ == VK_NULL_HANDLE) {
            if (!createSurfaceResources(window, w, h)) return false;
            if (fg_) fg_->forgetTargets();
        }

        if (!engineReady_) {
            const char* cache = std::getenv("POJAV_LSFG_CACHE");
            if (!cache || !*cache) {
                ZFG_LOGE("POJAV_LSFG_CACHE is not set");
                return false;
            }
            struct stat st{};
            if (stat(cache, &st) != 0 || st.st_size <= 0) {
                ZFG_LOGE("LSFG shader cache missing: %s", cache);
                return false;
            }
            if (!zlsfg_fill_dispatch(gipa_, instance_, gdpa_, device_)) {
                ZFG_LOGE("LSFG Vulkan dispatch init failed");
                return false;
            }
            fg_ = std::make_unique<lsfg::Engine>();
            if (!fg_->init(device_, physical_, cache, lsfg::kSpirv16)) {
                ZFG_LOGE("LSFG Engine::init failed");
                fg_.reset();
                return false;
            }
            engineReady_ = true;
        }

        if (engineW_ != w || engineH_ != h) {
            fg_->setGuestExtent(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
            if (!fg_->prepare(static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                              VK_FORMAT_R8G8B8A8_UNORM)) {
                ZFG_LOGE("LSFG prepare failed for %dx%d", w, h);
                return false;
            }
            if (!createFrameResources(w, h)) return false;
            engineW_ = w;
            engineH_ = h;
            previousCpu_.clear();
            fg_->reset();
        }

        ready_.store(true, std::memory_order_release);
        return true;
    }

    bool initVulkan() {
        void* vulkanHandle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        externalVulkanHandle_ = false;
        ZFG_LOGI("Using Android Vulkan loader %p", vulkanHandle);
        if (!vulkanHandle) {
            ZFG_LOGE("dlopen Vulkan failed: %s", dlerror());
            return false;
        }
        vulkanHandle_ = vulkanHandle;

        gipa_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vulkanHandle_, "vkGetInstanceProcAddr"));
        if (!gipa_) {
            ZFG_LOGE("vkGetInstanceProcAddr missing");
            return false;
        }

        auto CreateInstance = load_global<PFN_vkCreateInstance>(gipa_, "vkCreateInstance");
        auto EnumeratePhysicalDevices = static_cast<PFN_vkEnumeratePhysicalDevices>(nullptr);
        if (!CreateInstance) return false;

        const char* instanceExts[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
        };
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Zalith LSFG Native";
        app.applicationVersion = 1;
        app.pEngineName = "lsfg-native";
        app.engineVersion = 1;
        app.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = instanceExts;

        VkResult vr = CreateInstance(&ici, nullptr, &instance_);
        if (vr != VK_SUCCESS) {
            ZFG_LOGE("vkCreateInstance failed: %d", vr);
            return false;
        }

        EnumeratePhysicalDevices = load_instance<PFN_vkEnumeratePhysicalDevices>(gipa_, instance_, "vkEnumeratePhysicalDevices");
        auto GetQProps = load_instance<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            gipa_, instance_, "vkGetPhysicalDeviceQueueFamilyProperties");
        pGetSurfaceSupport_ = load_instance<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
            gipa_, instance_, "vkGetPhysicalDeviceSurfaceSupportKHR");
        pGetSurfaceFormats_ = load_instance<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
            gipa_, instance_, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        pGetSurfaceModes_ = load_instance<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
            gipa_, instance_, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        pCreateAndroidSurface_ = load_instance<PFN_vkCreateAndroidSurfaceKHR>(
            gipa_, instance_, "vkCreateAndroidSurfaceKHR");
        pDestroySurface_ = load_instance<PFN_vkDestroySurfaceKHR>(
            gipa_, instance_, "vkDestroySurfaceKHR");
        pCreateDevice_ = load_instance<PFN_vkCreateDevice>(gipa_, instance_, "vkCreateDevice");

        id_.GetInstanceProcAddr = gipa_;
        id_.DestroyInstance = load_instance<PFN_vkDestroyInstance>(gipa_, instance_, "vkDestroyInstance");
        id_.GetPhysicalDeviceMemoryProperties = load_instance<PFN_vkGetPhysicalDeviceMemoryProperties>(
            gipa_, instance_, "vkGetPhysicalDeviceMemoryProperties");
        id_.GetPhysicalDeviceQueueFamilyProperties = GetQProps;
        id_.GetPhysicalDeviceProperties = load_instance<PFN_vkGetPhysicalDeviceProperties>(
            gipa_, instance_, "vkGetPhysicalDeviceProperties");
        id_.GetPhysicalDeviceProperties2 = load_instance<PFN_vkGetPhysicalDeviceProperties2>(
            gipa_, instance_, "vkGetPhysicalDeviceProperties2");
        id_.GetPhysicalDeviceSurfaceCapabilitiesKHR = load_instance<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            gipa_, instance_, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

        if (!EnumeratePhysicalDevices || !GetQProps || !pCreateAndroidSurface_ || !pCreateDevice_) {
            ZFG_LOGE("required instance functions missing");
            return false;
        }

        uint32_t physicalCount = 0;
        if (EnumeratePhysicalDevices(instance_, &physicalCount, nullptr) != VK_SUCCESS || physicalCount == 0) {
            ZFG_LOGE("No Vulkan physical devices");
            return false;
        }
        std::vector<VkPhysicalDevice> physicals(physicalCount);
        EnumeratePhysicalDevices(instance_, &physicalCount, physicals.data());
        physical_ = physicals[0];

        uint32_t qCount = 0;
        GetQProps(physical_, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qProps(qCount);
        GetQProps(physical_, &qCount, qProps.data());
        queueFamily_ = UINT32_MAX;
        for (uint32_t i = 0; i < qCount; ++i) {
            if ((qProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                (qProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                queueFamily_ = i;
                break;
            }
        }
        if (queueFamily_ == UINT32_MAX) {
            ZFG_LOGE("No graphics+compute queue");
            return false;
        }

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        const char* deviceExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = deviceExts;

        vr = pCreateDevice_(physical_, &dci, nullptr, &device_);
        if (vr != VK_SUCCESS) {
            ZFG_LOGE("vkCreateDevice failed: %d", vr);
            return false;
        }

        gdpa_ = load_instance<PFN_vkGetDeviceProcAddr>(gipa_, instance_, "vkGetDeviceProcAddr");
        if (!gdpa_) return false;

#define LOAD_D(name) dd_.name = load_device<PFN_vk##name>(gdpa_, device_, "vk" #name)
        dd_.GetDeviceProcAddr = gdpa_;
        LOAD_D(DestroyDevice);
        LOAD_D(GetDeviceQueue);
        LOAD_D(QueueSubmit);
        LOAD_D(QueueSubmit2);
        LOAD_D(QueueSubmit2KHR);
        LOAD_D(QueueWaitIdle);
        LOAD_D(DeviceWaitIdle);
        LOAD_D(CreateSwapchainKHR);
        LOAD_D(DestroySwapchainKHR);
        LOAD_D(GetSwapchainImagesKHR);
        LOAD_D(AcquireNextImageKHR);
        LOAD_D(AcquireNextImage2KHR);
        LOAD_D(QueuePresentKHR);
        LOAD_D(CreateImage);
        LOAD_D(DestroyImage);
        LOAD_D(CreateImageView);
        LOAD_D(DestroyImageView);
        LOAD_D(AllocateMemory);
        LOAD_D(FreeMemory);
        LOAD_D(BindImageMemory);
        LOAD_D(GetImageMemoryRequirements);
        LOAD_D(CreateBuffer);
        LOAD_D(DestroyBuffer);
        LOAD_D(GetBufferMemoryRequirements);
        LOAD_D(BindBufferMemory);
        LOAD_D(MapMemory);
        LOAD_D(UnmapMemory);
        LOAD_D(CreateSampler);
        LOAD_D(DestroySampler);
        LOAD_D(CreateShaderModule);
        LOAD_D(DestroyShaderModule);
        LOAD_D(CreateDescriptorSetLayout);
        LOAD_D(DestroyDescriptorSetLayout);
        LOAD_D(CreatePipelineLayout);
        LOAD_D(DestroyPipelineLayout);
        LOAD_D(CreateComputePipelines);
        LOAD_D(DestroyPipeline);
        LOAD_D(CreateDescriptorPool);
        LOAD_D(DestroyDescriptorPool);
        LOAD_D(ResetDescriptorPool);
        LOAD_D(AllocateDescriptorSets);
        LOAD_D(UpdateDescriptorSets);
        LOAD_D(CreateCommandPool);
        LOAD_D(DestroyCommandPool);
        LOAD_D(AllocateCommandBuffers);
        LOAD_D(FreeCommandBuffers);
        LOAD_D(BeginCommandBuffer);
        LOAD_D(EndCommandBuffer);
        LOAD_D(CmdBindPipeline);
        LOAD_D(CmdBindDescriptorSets);
        LOAD_D(CmdDispatch);
        LOAD_D(CmdPipelineBarrier);
        LOAD_D(CmdCopyImage);
        LOAD_D(CmdCopyImageToBuffer);
        LOAD_D(CmdBlitImage);
        LOAD_D(CmdClearColorImage);
        LOAD_D(CreateFence);
        LOAD_D(DestroyFence);
        LOAD_D(WaitForFences);
        LOAD_D(ResetFences);
        LOAD_D(CreateSemaphore);
        LOAD_D(DestroySemaphore);
#undef LOAD_D

        pCmdCopyBufferToImage_ = load_device<PFN_vkCmdCopyBufferToImage>(gdpa_, device_, "vkCmdCopyBufferToImage");
        pResetCommandPool_ = load_device<PFN_vkResetCommandPool>(gdpa_, device_, "vkResetCommandPool");

        if (!dd_.GetDeviceQueue || !dd_.QueueSubmit || !dd_.QueuePresentKHR ||
            !dd_.AcquireNextImageKHR || !pCmdCopyBufferToImage_ || !pResetCommandPool_) {
            ZFG_LOGE("required device functions missing");
            return false;
        }

        dd_.GetDeviceQueue(device_, queueFamily_, 0, &queue_);

        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = queueFamily_;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (dd_.CreateCommandPool(device_, &cpci, nullptr, &commandPool_) != VK_SUCCESS) return false;

        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = commandPool_;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (dd_.AllocateCommandBuffers(device_, &cbai, &commandBuffer_) != VK_SUCCESS) return false;

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (dd_.CreateFence(device_, &fci, nullptr, &fence_) != VK_SUCCESS) return false;

        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (dd_.CreateSemaphore(device_, &sci, nullptr, &acquireSemaphore_) != VK_SUCCESS) return false;
        if (dd_.CreateSemaphore(device_, &sci, nullptr, &renderSemaphore_) != VK_SUCCESS) return false;

        vkReady_ = true;
        ZFG_LOGI("LSFG Vulkan presenter initialized");
        return true;
    }

    bool createSurfaceResources(ANativeWindow* window, int w, int h) {
        destroySurfaceResources();
        if (!window) return false;

        ANativeWindow_acquire(window);
        window_ = window;

        VkAndroidSurfaceCreateInfoKHR asci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        asci.window = window_;
        if (pCreateAndroidSurface_(instance_, &asci, nullptr, &surface_) != VK_SUCCESS) {
            ZFG_LOGE("vkCreateAndroidSurfaceKHR failed");
            return false;
        }

        VkBool32 supported = VK_FALSE;
        if (!pGetSurfaceSupport_ ||
            pGetSurfaceSupport_(physical_, queueFamily_, surface_, &supported) != VK_SUCCESS ||
            !supported) {
            ZFG_LOGE("Selected queue cannot present to Android surface");
            return false;
        }

        uint32_t formatCount = 0;
        if (pGetSurfaceFormats_(physical_, surface_, &formatCount, nullptr) != VK_SUCCESS || !formatCount)
            return false;
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        pGetSurfaceFormats_(physical_, surface_, &formatCount, formats.data());
        surfaceFormat_ = formats[0];
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) {
                surfaceFormat_ = f;
                break;
            }
        }

        VkSurfaceCapabilitiesKHR caps{};
        if (id_.GetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) != VK_SUCCESS)
            return false;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            ZFG_LOGE("Surface does not support TRANSFER_DST swapchain images");
            return false;
        }

        VkExtent2D extent{};
        if (caps.currentExtent.width != UINT32_MAX) {
            extent = caps.currentExtent;
        } else {
            extent.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
        }

        uint32_t modeCount = 0;
        pGetSurfaceModes_(physical_, surface_, &modeCount, nullptr);
        std::vector<VkPresentModeKHR> modes(modeCount);
        if (modeCount) pGetSurfaceModes_(physical_, surface_, &modeCount, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        for (auto m : modes) {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
                mode = m;
                break;
            }
        }

        uint32_t minCount = std::max(caps.minImageCount, 3u);
        if (caps.maxImageCount && minCount > caps.maxImageCount) minCount = caps.maxImageCount;

        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = surface_;
        sci.minImageCount = minCount;
        sci.imageFormat = surfaceFormat_.format;
        sci.imageColorSpace = surfaceFormat_.colorSpace;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = mode;
        sci.clipped = VK_TRUE;

        if (dd_.CreateSwapchainKHR(device_, &sci, nullptr, &swapchain_) != VK_SUCCESS) {
            ZFG_LOGE("vkCreateSwapchainKHR failed");
            return false;
        }

        uint32_t imageCount = 0;
        dd_.GetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        swapImages_.resize(imageCount);
        dd_.GetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapImages_.data());

        swapExtent_ = extent;
        surfaceW_ = w;
        surfaceH_ = h;
        ZFG_LOGI("Surface ready %ux%u format=%d images=%u mode=%d",
                 extent.width, extent.height, static_cast<int>(surfaceFormat_.format),
                 imageCount, static_cast<int>(mode));
        return true;
    }

    void destroySurfaceResources() {
        if (!vkReady_ || device_ == VK_NULL_HANDLE) {
            if (window_) {
                ANativeWindow_release(window_);
                window_ = nullptr;
            }
            return;
        }
        if (dd_.DeviceWaitIdle) dd_.DeviceWaitIdle(device_);
        if (swapchain_ && dd_.DestroySwapchainKHR) {
            dd_.DestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        swapImages_.clear();
        if (surface_ && pDestroySurface_) {
            pDestroySurface_(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (window_) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
        surfaceW_ = surfaceH_ = 0;
        history_ = 0;
        ready_.store(false, std::memory_order_release);
    }

    uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties props{};
        id_.GetPhysicalDeviceMemoryProperties(physical_, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        }
        return UINT32_MAX;
    }

    bool createImage(Image& out, int w, int h, VkImageUsageFlags usage) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (dd_.CreateImage(device_, &ii, nullptr, &out.image) != VK_SUCCESS) return false;

        VkMemoryRequirements req{};
        dd_.GetImageMemoryRequirements(device_, out.image, &req);
        uint32_t type = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        if (dd_.AllocateMemory(device_, &mai, nullptr, &out.memory) != VK_SUCCESS) return false;
        if (dd_.BindImageMemory(device_, out.image, out.memory, 0) != VK_SUCCESS) return false;

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = out.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (dd_.CreateImageView(device_, &vi, nullptr, &out.view) != VK_SUCCESS) return false;
        return true;
    }

    void destroyImage(Image& i) {
        if (i.view) dd_.DestroyImageView(device_, i.view, nullptr);
        if (i.image) dd_.DestroyImage(device_, i.image, nullptr);
        if (i.memory) dd_.FreeMemory(device_, i.memory, nullptr);
        i = Image{};
    }

    bool createFrameResources(int w, int h) {
        if (device_ == VK_NULL_HANDLE) return false;
        dd_.DeviceWaitIdle(device_);

        destroyImage(source_[0]);
        destroyImage(source_[1]);
        destroyImage(generatedImage_);

        if (stagingMapped_) {
            dd_.UnmapMemory(device_, stagingMemory_);
            stagingMapped_ = nullptr;
        }
        if (stagingBuffer_) dd_.DestroyBuffer(device_, stagingBuffer_, nullptr);
        if (stagingMemory_) dd_.FreeMemory(device_, stagingMemory_, nullptr);
        stagingBuffer_ = VK_NULL_HANDLE;
        stagingMemory_ = VK_NULL_HANDLE;

        const VkImageUsageFlags sourceUsage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!createImage(source_[0], w, h, sourceUsage)) return false;
        if (!createImage(source_[1], w, h, sourceUsage)) return false;
        if (!createImage(generatedImage_, w, h,
                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return false;

        stagingSize_ = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * 4u;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = stagingSize_;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (dd_.CreateBuffer(device_, &bi, nullptr, &stagingBuffer_) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        dd_.GetBufferMemoryRequirements(device_, stagingBuffer_, &req);
        uint32_t type = findMemoryType(
            req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        if (dd_.AllocateMemory(device_, &mai, nullptr, &stagingMemory_) != VK_SUCCESS) return false;
        if (dd_.BindBufferMemory(device_, stagingBuffer_, stagingMemory_, 0) != VK_SUCCESS) return false;
        if (dd_.MapMemory(device_, stagingMemory_, 0, stagingSize_, 0, &stagingMapped_) != VK_SUCCESS)
            return false;

        source_[0].valid = false;
        source_[1].valid = false;
        generatedImage_.valid = true;
        generatedLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    VkImageMemoryBarrier imageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                      VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return b;
    }

    bool beginCommands() {
        if (dd_.WaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
        if (dd_.ResetFences(device_, 1, &fence_) != VK_SUCCESS) return false;
        if (pResetCommandPool_(device_, commandPool_, 0) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return dd_.BeginCommandBuffer(commandBuffer_, &bi) == VK_SUCCESS;
    }

    bool submitCommands(VkSemaphore waitSem = VK_NULL_HANDLE, VkSemaphore signalSem = VK_NULL_HANDLE) {
        if (dd_.EndCommandBuffer(commandBuffer_) != VK_SUCCESS) return false;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &commandBuffer_;
        if (waitSem) {
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &waitSem;
            si.pWaitDstStageMask = &stage;
        }
        if (signalSem) {
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &signalSem;
        }
        if (dd_.QueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) return false;
        return true;
    }

    bool uploadFrame(Image& dst, const std::vector<uint8_t>& frame, int w, int h) {
        if (frame.size() < static_cast<size_t>(stagingSize_)) return false;
        std::memcpy(stagingMapped_, frame.data(), static_cast<size_t>(stagingSize_));

        if (!beginCommands()) return false;

        VkImageMemoryBarrier toDst = imageBarrier(
            dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT
        );
        dd_.CmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        pCmdCopyBufferToImage_(commandBuffer_, stagingBuffer_, dst.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier toRead = imageBarrier(
            dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT
        );
        dd_.CmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 0, nullptr, 0, nullptr, 1, &toRead);

        if (!submitCommands()) return false;
        if (dd_.WaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
        dst.valid = true;
        return true;
    }

    bool presentImage(Image& src, bool sourceIsShaderRead, int w, int h) {
        uint32_t swapIndex = 0;
        VkResult ar = dd_.AcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                              acquireSemaphore_, VK_NULL_HANDLE, &swapIndex);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!createSurfaceResources(window_, surfaceW_, surfaceH_)) return false;
            ar = dd_.AcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                         acquireSemaphore_, VK_NULL_HANDLE, &swapIndex);
        }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return false;
        if (swapIndex >= swapImages_.size()) return false;

        if (!beginCommands()) return false;

        VkImageLayout srcOld = sourceIsShaderRead
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_GENERAL;
        VkImageMemoryBarrier srcToCopy = imageBarrier(
            src.image, srcOld, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            sourceIsShaderRead ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT
        );
        VkImageMemoryBarrier dstToCopy = imageBarrier(
            swapImages_[swapIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT
        );
        VkImageMemoryBarrier pre[] = {srcToCopy, dstToCopy};
        dd_.CmdPipelineBarrier(commandBuffer_,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 0, nullptr, 0, nullptr, 2, pre);

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {w, h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {
            static_cast<int32_t>(swapExtent_.width),
            static_cast<int32_t>(swapExtent_.height),
            1
        };
        dd_.CmdBlitImage(commandBuffer_,
                         src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         swapImages_[swapIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &blit, VK_FILTER_NEAREST);

        VkImageMemoryBarrier dstPresent = imageBarrier(
            swapImages_[swapIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0
        );
        VkImageMemoryBarrier srcRestore = imageBarrier(
            src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcOld,
            VK_ACCESS_TRANSFER_READ_BIT,
            sourceIsShaderRead ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT
        );
        VkImageMemoryBarrier post[] = {dstPresent, srcRestore};
        dd_.CmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0, 0, nullptr, 0, nullptr, 2, post);

        if (!submitCommands(acquireSemaphore_, renderSemaphore_)) return false;

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderSemaphore_;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &swapIndex;
        VkResult pr = dd_.QueuePresentKHR(queue_, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR && pr != VK_ERROR_OUT_OF_DATE_KHR)
            return false;
        if (dd_.WaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
        presented_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool synthesizeAndPresent(int generation, int count, int w, int h) {
        if (!fg_) return false;
        if (!beginCommands()) return false;

        if (generatedLayout_ == VK_IMAGE_LAYOUT_UNDEFINED) {
            VkImageMemoryBarrier init = imageBarrier(
                generatedImage_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                0, VK_ACCESS_SHADER_WRITE_BIT
            );
            dd_.CmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   0, 0, nullptr, 0, nullptr, 1, &init);
            generatedLayout_ = VK_IMAGE_LAYOUT_GENERAL;
        }

        fg_->generateInto(
            commandBuffer_,
            static_cast<uint32_t>(generation),
            static_cast<uint32_t>(generation),
            generatedImage_.image,
            generatedImage_.view,
            static_cast<uint32_t>(w),
            static_cast<uint32_t>(h)
        );

        if (!submitCommands()) return false;
        if (dd_.WaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;

        if (!presentImage(generatedImage_, false, w, h)) return false;
        generated_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    double sceneDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                           int w, int h) {
        if (a.size() != b.size() || a.empty()) return 1.0;
        uint64_t sum = 0, count = 0;
        const int stepY = 8;
        const int stepX = 16;
        for (int y = 0; y < h; y += stepY) {
            const size_t row = static_cast<size_t>(y) * w * 4u;
            for (int x = 0; x < w; x += stepX) {
                const size_t i = row + static_cast<size_t>(x) * 4u;
                sum += static_cast<uint64_t>(std::abs(int(a[i]) - int(b[i])));
                sum += static_cast<uint64_t>(std::abs(int(a[i + 1]) - int(b[i + 1])));
                sum += static_cast<uint64_t>(std::abs(int(a[i + 2]) - int(b[i + 2])));
                count += 3;
            }
        }
        return count ? static_cast<double>(sum) / (static_cast<double>(count) * 255.0) : 0.0;
    }

    bool processFrame(const std::vector<uint8_t>& frame, int w, int h, const SubmitConfig& cfg) {
        if (!engineReady_ || !fg_ || !swapchain_) return false;

        const float flowScale =
            cfg.perfPreset <= 0 ? 1.0f :
            (cfg.perfPreset >= 2 ? 0.50f : 0.75f);

        fg_->setGuestExtent(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        fg_->configure(
            static_cast<uint32_t>(std::clamp(cfg.multiplier, 2, 4)),
            0,
            flowScale,
            static_cast<float>(std::clamp(cfg.targetFps, 30, 120))
        );

        Image& curr = source_[0];
        if (!uploadFrame(curr, frame, w, h)) return false;

        bool allowGeneration = true;
        if (cfg.sceneProtection && !previousCpu_.empty()) {
            const double diff = sceneDifference(previousCpu_, frame, w, h);
            if (diff > 0.30) allowGeneration = false;
        }

        uint32_t genCount = fg_->plan(
            static_cast<uint32_t>(lsfg::kMaxGenerations),
            realSubmitted_.load(std::memory_order_relaxed)
        );
        if (!allowGeneration) genCount = 0;

        if (!beginCommands()) return false;
        fg_->process(
            commandBuffer_,
            curr.image,
            static_cast<uint32_t>(w),
            static_cast<uint32_t>(h),
            genCount
        );
        if (!submitCommands()) return false;
        if (dd_.WaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;

        const int targetFps = std::clamp(cfg.targetFps, 30, 120);
        const auto interval = std::chrono::microseconds(1000000 / targetFps);

        for (uint32_t g = 0; g < genCount; ++g) {
            auto started = std::chrono::steady_clock::now();
            if (!synthesizeAndPresent(static_cast<int>(g), static_cast<int>(genCount), w, h))
                return false;
            auto spent = std::chrono::steady_clock::now() - started;
            if (spent < interval) std::this_thread::sleep_for(interval - spent);
        }

        if (!presentImage(curr, false, w, h)) return false;

        previousCpu_ = frame;
        return true;
    }

    void destroyAll() {
        if (device_ != VK_NULL_HANDLE && dd_.DeviceWaitIdle) dd_.DeviceWaitIdle(device_);
        destroySurfaceResources();

        if (engineReady_) {
            fg_.reset();
            engineReady_ = false;
        }
        if (device_ != VK_NULL_HANDLE) {
            destroyImage(source_[0]);
            destroyImage(source_[1]);
            destroyImage(generatedImage_);

            if (stagingMapped_ && dd_.UnmapMemory) dd_.UnmapMemory(device_, stagingMemory_);
            stagingMapped_ = nullptr;
            if (stagingBuffer_ && dd_.DestroyBuffer) dd_.DestroyBuffer(device_, stagingBuffer_, nullptr);
            if (stagingMemory_ && dd_.FreeMemory) dd_.FreeMemory(device_, stagingMemory_, nullptr);
            stagingBuffer_ = VK_NULL_HANDLE;
            stagingMemory_ = VK_NULL_HANDLE;

            if (acquireSemaphore_ && dd_.DestroySemaphore) dd_.DestroySemaphore(device_, acquireSemaphore_, nullptr);
            if (renderSemaphore_ && dd_.DestroySemaphore) dd_.DestroySemaphore(device_, renderSemaphore_, nullptr);
            if (fence_ && dd_.DestroyFence) dd_.DestroyFence(device_, fence_, nullptr);
            if (commandPool_ && dd_.DestroyCommandPool) dd_.DestroyCommandPool(device_, commandPool_, nullptr);

            if (dd_.DestroyDevice) dd_.DestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }

        if (instance_ != VK_NULL_HANDLE && id_.DestroyInstance) {
            id_.DestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        if (vulkanHandle_ && !externalVulkanHandle_) {
            dlclose(vulkanHandle_);
        }
        vulkanHandle_ = nullptr;
        vkReady_ = false;
        ready_.store(false, std::memory_order_release);
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable releaseCv_;
    std::thread worker_;
    bool workerStarted_ = false;
    bool stop_ = false;

    std::vector<uint8_t> pending_;
    ANativeWindow* pendingWindow_ = nullptr;
    int pendingW_ = 0;
    int pendingH_ = 0;
    SubmitConfig pendingCfg_{};
    uint64_t pendingSequence_ = 0;
    uint64_t releaseRequest_ = 0;
    uint64_t releaseAck_ = 0;

    std::atomic<bool> ready_{false};
    std::atomic<bool> fatal_{false};
    std::atomic<uint64_t> realSubmitted_{0};
    std::atomic<uint64_t> generated_{0};
    std::atomic<uint64_t> presented_{0};

    void* vulkanHandle_ = nullptr;
    bool externalVulkanHandle_ = false;
    PFN_vkGetInstanceProcAddr gipa_ = nullptr;
    PFN_vkGetDeviceProcAddr gdpa_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    bool vkReady_ = false;

    winfg::InstanceDispatch id_{};
    winfg::DeviceDispatch dd_{};
    std::unique_ptr<lsfg::Engine> fg_;
    bool engineReady_ = false;

    PFN_vkCreateDevice pCreateDevice_ = nullptr;
    PFN_vkCreateAndroidSurfaceKHR pCreateAndroidSurface_ = nullptr;
    PFN_vkDestroySurfaceKHR pDestroySurface_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR pGetSurfaceSupport_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR pGetSurfaceFormats_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR pGetSurfaceModes_ = nullptr;
    PFN_vkCmdCopyBufferToImage pCmdCopyBufferToImage_ = nullptr;
    PFN_vkResetCommandPool pResetCommandPool_ = nullptr;

    ANativeWindow* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surfaceFormat_{};
    VkExtent2D swapExtent_{};
    std::vector<VkImage> swapImages_;
    int surfaceW_ = 0;
    int surfaceH_ = 0;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore acquireSemaphore_ = VK_NULL_HANDLE;
    VkSemaphore renderSemaphore_ = VK_NULL_HANDLE;

    Image source_[2];
    Image generatedImage_;
    VkImageLayout generatedLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    int engineW_ = 0;
    int engineH_ = 0;

    VkBuffer stagingBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    void* stagingMapped_ = nullptr;
    VkDeviceSize stagingSize_ = 0;

    std::vector<uint8_t> previousCpu_;
};

Presenter gPresenter;

} // namespace

extern "C" int zlsfg_validate_dll(const char* dllPath) {
    if (!dllPath || !*dllPath) return static_cast<int>(lsfg::DllStatus::NotInstalled);
    return static_cast<int>(lsfg::validateDll(dllPath));
}

extern "C" int zlsfg_build_cache(const char* dllPath, const char* cachePath, int preferFp16) {
    if (!dllPath || !cachePath) return static_cast<int>(lsfg::DllStatus::NotInstalled);
    return static_cast<int>(lsfg::buildCache(dllPath, cachePath, preferFp16 != 0));
}

extern "C" int zlsfg_cache_matches(const char* cachePath, const char* dllPath) {
    if (!cachePath || !dllPath) return 0;
    bool matches = false;
    const auto st = lsfg::cacheMatchesSource(cachePath, dllPath, matches);
    return st == lsfg::DllStatus::Ok && matches ? 1 : 0;
}

extern "C" const char* zlsfg_status_name(int status) {
    return lsfg::statusName(static_cast<lsfg::DllStatus>(status));
}

extern "C" int zlsfg_submit_frame(
    ANativeWindow* window,
    const void* pixels,
    int width,
    int height,
    int stride,
    int multiplier,
    int perfPreset,
    int targetFps,
    int lowLatency,
    int sceneProtection
) {
    SubmitConfig cfg;
    cfg.multiplier = multiplier;
    cfg.perfPreset = perfPreset;
    cfg.targetFps = targetFps;
    cfg.lowLatency = lowLatency != 0;
    cfg.sceneProtection = sceneProtection != 0;
    return gPresenter.submit(window, pixels, width, height, stride, cfg) ? 1 : 0;
}

extern "C" void zlsfg_release_surface() {
    gPresenter.releaseSurfaceSync();
}

extern "C" void zlsfg_get_stats(uint64_t out[4]) {
    if (!out) return;
    gPresenter.stats(out);
}

extern "C" const char* zlsfg_backend_name() {
    return "LSFG Native from user Lossless.dll";
}
