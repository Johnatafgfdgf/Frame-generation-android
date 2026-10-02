#include <jni.h>
#include <volk.h>
#include <vulkan/vulkan_core.h>

#include <lsfg_3_1.hpp>
#include <lsfg_3_1p.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint32_t kRtRcData = 10u;
constexpr uint32_t kFirstFp32Spirv = 353u;
constexpr uint32_t kLastFp32Spirv = 400u;

struct Section {
    uint32_t virtualAddress{};
    uint32_t virtualSize{};
    uint32_t rawOffset{};
    uint32_t rawSize{};
};

struct BackendState {
    bool prepared = false;
    bool performance = false;
    int multiplier = 2;
    uint64_t deviceKey = 0;
    std::string dllPath;
    std::unordered_map<uint32_t, std::vector<uint8_t>> shaders;
};

BackendState gState;
std::mutex gMutex;

const std::unordered_map<std::string, uint32_t> kBaseShaderIds = {
    {"mipmaps", 255},
    {"alpha[0]", 267},
    {"alpha[1]", 268},
    {"alpha[2]", 269},
    {"alpha[3]", 270},
    {"beta[0]", 275},
    {"beta[1]", 276},
    {"beta[2]", 277},
    {"beta[3]", 278},
    {"beta[4]", 279},
    {"gamma[0]", 257},
    {"gamma[1]", 259},
    {"gamma[2]", 260},
    {"gamma[3]", 261},
    {"gamma[4]", 262},
    {"delta[0]", 257},
    {"delta[1]", 263},
    {"delta[2]", 264},
    {"delta[3]", 265},
    {"delta[4]", 266},
    {"delta[5]", 258},
    {"delta[6]", 271},
    {"delta[7]", 272},
    {"delta[8]", 273},
    {"delta[9]", 274},
    {"generate", 256},

    {"p_mipmaps", 255},
    {"p_alpha[0]", 290},
    {"p_alpha[1]", 291},
    {"p_alpha[2]", 292},
    {"p_alpha[3]", 293},
    {"p_beta[0]", 298},
    {"p_beta[1]", 299},
    {"p_beta[2]", 300},
    {"p_beta[3]", 301},
    {"p_beta[4]", 302},
    {"p_gamma[0]", 280},
    {"p_gamma[1]", 282},
    {"p_gamma[2]", 283},
    {"p_gamma[3]", 284},
    {"p_gamma[4]", 285},
    {"p_delta[0]", 280},
    {"p_delta[1]", 286},
    {"p_delta[2]", 287},
    {"p_delta[3]", 288},
    {"p_delta[4]", 289},
    {"p_delta[5]", 281},
    {"p_delta[6]", 294},
    {"p_delta[7]", 295},
    {"p_delta[8]", 296},
    {"p_delta[9]", 297},
    {"p_generate", 256},
};

jstring toJString(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

std::string fromJString(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }

    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return {};
    }

    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

uint16_t read16(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 2 > bytes.size()) {
        throw std::runtime_error("Unexpected end of PE file (u16).");
    }

    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(bytes[offset + 1] << 8);
}

uint32_t read32(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("Unexpected end of PE file (u32).");
    }

    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("Could not open Lossless.dll.");
    }

    const std::streamsize size = input.tellg();
    if (size <= 0 || size > 256 * 1024 * 1024) {
        throw std::runtime_error("Lossless.dll has an invalid size.");
    }

    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!input.read(
            reinterpret_cast<char*>(bytes.data()),
            size)) {
        throw std::runtime_error("Could not read Lossless.dll.");
    }

    return bytes;
}

std::optional<size_t> rvaToOffset(
        uint32_t rva,
        const std::vector<Section>& sections,
        size_t fileSize) {
    for (const auto& section : sections) {
        const uint32_t span =
                std::max(section.virtualSize, section.rawSize);

        if (rva < section.virtualAddress ||
            rva >= section.virtualAddress + span) {
            continue;
        }

        const uint64_t offset =
                static_cast<uint64_t>(section.rawOffset) +
                (rva - section.virtualAddress);

        if (offset >= fileSize) {
            return std::nullopt;
        }

        return static_cast<size_t>(offset);
    }

    return std::nullopt;
}

struct ResourceDirectoryEntry {
    uint32_t id{};
    bool isNamed = false;
    bool isDirectory = false;
    uint32_t relativeOffset{};
};

std::vector<ResourceDirectoryEntry> readResourceDirectory(
        const std::vector<uint8_t>& bytes,
        size_t resourceBase,
        uint32_t relativeOffset) {
    const uint64_t directoryOffset64 =
            static_cast<uint64_t>(resourceBase) +
            relativeOffset;

    if (directoryOffset64 + 16 > bytes.size()) {
        throw std::runtime_error(
                "Resource directory points outside Lossless.dll.");
    }

    const size_t directoryOffset =
            static_cast<size_t>(directoryOffset64);

    const uint16_t namedCount =
            read16(bytes, directoryOffset + 12);
    const uint16_t idCount =
            read16(bytes, directoryOffset + 14);

    const size_t total =
            static_cast<size_t>(namedCount) + idCount;

    if (directoryOffset + 16 + total * 8 > bytes.size()) {
        throw std::runtime_error(
                "Invalid PE resource directory entry table.");
    }

    std::vector<ResourceDirectoryEntry> result;
    result.reserve(total);

    for (size_t i = 0; i < total; ++i) {
        const size_t entryOffset =
                directoryOffset + 16 + i * 8;

        const uint32_t name = read32(bytes, entryOffset);
        const uint32_t child = read32(bytes, entryOffset + 4);

        result.push_back(ResourceDirectoryEntry{
            .id = name & 0x7fffffffu,
            .isNamed = (name & 0x80000000u) != 0,
            .isDirectory = (child & 0x80000000u) != 0,
            .relativeOffset = child & 0x7fffffffu,
        });
    }

    return result;
}

std::optional<ResourceDirectoryEntry> findNumericResource(
        const std::vector<ResourceDirectoryEntry>& entries,
        uint32_t id) {
    for (const auto& entry : entries) {
        if (!entry.isNamed && entry.id == id) {
            return entry;
        }
    }

    return std::nullopt;
}

uint32_t resolveDataEntryRelativeOffset(
        const std::vector<uint8_t>& bytes,
        size_t resourceBase,
        ResourceDirectoryEntry entry) {
    // Resource tree normally is:
    // type -> numeric resource id -> language -> IMAGE_RESOURCE_DATA_ENTRY.
    // Follow directory nodes until the first data entry is reached.
    for (int depth = 0; depth < 4; ++depth) {
        if (!entry.isDirectory) {
            return entry.relativeOffset;
        }

        const auto children =
                readResourceDirectory(
                        bytes,
                        resourceBase,
                        entry.relativeOffset);

        if (children.empty()) {
            throw std::runtime_error(
                    "Empty PE resource directory.");
        }

        entry = children.front();
    }

    throw std::runtime_error(
            "PE resource tree is deeper than expected.");
}

std::unordered_map<uint32_t, std::vector<uint8_t>>
extractFp32SpirvResources(const std::string& dllPath) {
    const std::vector<uint8_t> bytes = readFile(dllPath);

    if (bytes.size() < 0x100 ||
        read16(bytes, 0) != 0x5a4d) {
        throw std::runtime_error(
                "Lossless.dll is not an MZ executable.");
    }

    const uint32_t peOffset = read32(bytes, 0x3c);
    if (static_cast<uint64_t>(peOffset) + 24 > bytes.size() ||
        read32(bytes, peOffset) != 0x00004550u) {
        throw std::runtime_error(
                "Lossless.dll has no valid PE signature.");
    }

    const uint16_t machine =
            read16(bytes, peOffset + 4);
    if (machine != 0x8664) {
        throw std::runtime_error(
                "Lossless.dll is not x86-64.");
    }

    const uint16_t sectionCount =
            read16(bytes, peOffset + 6);
    const uint16_t optionalSize =
            read16(bytes, peOffset + 20);

    const size_t optionalOffset =
            static_cast<size_t>(peOffset) + 24;

    if (optionalOffset + optionalSize > bytes.size()) {
        throw std::runtime_error(
                "Invalid PE optional header.");
    }

    const uint16_t magic =
            read16(bytes, optionalOffset);

    size_t dataDirectoryOffset = 0;
    if (magic == 0x20b) {
        dataDirectoryOffset = optionalOffset + 112;
    } else if (magic == 0x10b) {
        dataDirectoryOffset = optionalOffset + 96;
    } else {
        throw std::runtime_error(
                "Unsupported PE optional-header format.");
    }

    // IMAGE_DIRECTORY_ENTRY_RESOURCE == 2.
    const size_t resourceDirectoryEntry =
            dataDirectoryOffset + 2 * 8;

    if (resourceDirectoryEntry + 8 > bytes.size()) {
        throw std::runtime_error(
                "PE has no resource data-directory entry.");
    }

    const uint32_t resourceRva =
            read32(bytes, resourceDirectoryEntry);
    const uint32_t resourceSize =
            read32(bytes, resourceDirectoryEntry + 4);

    if (resourceRva == 0 || resourceSize == 0) {
        throw std::runtime_error(
                "Lossless.dll has no PE resource table.");
    }

    const size_t sectionTable =
            optionalOffset + optionalSize;

    if (sectionTable +
        static_cast<size_t>(sectionCount) * 40 >
        bytes.size()) {
        throw std::runtime_error(
                "Invalid PE section table.");
    }

    std::vector<Section> sections;
    sections.reserve(sectionCount);

    for (uint16_t i = 0; i < sectionCount; ++i) {
        const size_t offset =
                sectionTable + static_cast<size_t>(i) * 40;

        sections.push_back(Section{
            .virtualAddress = read32(bytes, offset + 12),
            .virtualSize = read32(bytes, offset + 8),
            .rawOffset = read32(bytes, offset + 20),
            .rawSize = read32(bytes, offset + 16),
        });
    }

    const auto resourceBaseOptional =
            rvaToOffset(
                    resourceRva,
                    sections,
                    bytes.size());

    if (!resourceBaseOptional.has_value()) {
        throw std::runtime_error(
                "Could not map PE resource RVA.");
    }

    const size_t resourceBase =
            *resourceBaseOptional;

    const auto root =
            readResourceDirectory(
                    bytes,
                    resourceBase,
                    0);

    const auto rcData =
            findNumericResource(root, kRtRcData);

    if (!rcData.has_value() ||
        !rcData->isDirectory) {
        throw std::runtime_error(
                "Lossless.dll has no RCDATA resource directory.");
    }

    const auto resources =
            readResourceDirectory(
                    bytes,
                    resourceBase,
                    rcData->relativeOffset);

    std::unordered_map<uint32_t, std::vector<uint8_t>>
            shaders;

    for (uint32_t id = kFirstFp32Spirv;
         id <= kLastFp32Spirv;
         ++id) {
        const auto resource =
                findNumericResource(resources, id);

        if (!resource.has_value()) {
            std::ostringstream error;
            error << "Missing Lossless Scaling shader resource "
                  << id << ".";
            throw std::runtime_error(error.str());
        }

        const uint32_t dataEntryRelative =
                resolveDataEntryRelativeOffset(
                        bytes,
                        resourceBase,
                        *resource);

        const uint64_t dataEntryOffset64 =
                static_cast<uint64_t>(resourceBase) +
                dataEntryRelative;

        if (dataEntryOffset64 + 16 > bytes.size()) {
            throw std::runtime_error(
                    "Invalid PE resource data entry.");
        }

        const size_t dataEntryOffset =
                static_cast<size_t>(dataEntryOffset64);

        const uint32_t dataRva =
                read32(bytes, dataEntryOffset);
        const uint32_t dataSize =
                read32(bytes, dataEntryOffset + 4);

        const auto fileOffset =
                rvaToOffset(
                        dataRva,
                        sections,
                        bytes.size());

        if (!fileOffset.has_value() ||
            static_cast<uint64_t>(*fileOffset) +
                    dataSize >
            bytes.size() ||
            dataSize < 4 ||
            (dataSize % 4) != 0) {
            throw std::runtime_error(
                    "Invalid SPIR-V resource range.");
        }

        if (read32(bytes, *fileOffset) != kSpirvMagic) {
            std::ostringstream error;
            error << "Resource " << id
                  << " is not SPIR-V.";
            throw std::runtime_error(error.str());
        }

        shaders[id] = std::vector<uint8_t>(
                bytes.begin() +
                        static_cast<std::ptrdiff_t>(*fileOffset),
                bytes.begin() +
                        static_cast<std::ptrdiff_t>(
                                *fileOffset + dataSize));
    }

    return shaders;
}

uint32_t fp32ResourceIdForName(
        const std::string& name) {
    const auto it = kBaseShaderIds.find(name);
    if (it == kBaseShaderIds.end()) {
        return 0;
    }

    return it->second + 98u;
}

std::vector<uint8_t> loadPreparedShader(
        const std::string& name) {
    std::lock_guard<std::mutex> lock(gMutex);

    const uint32_t resourceId =
            fp32ResourceIdForName(name);

    const auto it = gState.shaders.find(resourceId);
    if (resourceId == 0 ||
        it == gState.shaders.end()) {
        throw std::runtime_error(
                "Prepared shader not found: " + name);
    }

    return it->second;
}

uint64_t probeDeviceKey() {
    VkResult result = volkInitialize();
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
                "volkInitialize failed.");
    }

    VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "FrameGenLosslessProbe",
        .applicationVersion = VK_MAKE_VERSION(0, 3, 0),
        .pEngineName = "FrameGenLosslessBackend",
        .engineVersion = VK_MAKE_VERSION(0, 3, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };

    VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
    };

    VkInstance instance = VK_NULL_HANDLE;
    result = vkCreateInstance(
            &createInfo,
            nullptr,
            &instance);

    if (result != VK_SUCCESS ||
        instance == VK_NULL_HANDLE) {
        throw std::runtime_error(
                "Could not create Vulkan instance for LSFG.");
    }

    volkLoadInstanceOnly(instance);

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(
            instance,
            &deviceCount,
            nullptr);

    if (result != VK_SUCCESS ||
        deviceCount == 0) {
        vkDestroyInstance(instance, nullptr);
        throw std::runtime_error(
                "No Vulkan GPU found for LSFG.");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = vkEnumeratePhysicalDevices(
            instance,
            &deviceCount,
            devices.data());

    if (result != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        throw std::runtime_error(
                "Could not enumerate Vulkan GPUs.");
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(
            devices.front(),
            &properties);

    const uint64_t key =
            (static_cast<uint64_t>(properties.vendorID) << 32) |
            properties.deviceID;

    vkDestroyInstance(instance, nullptr);
    return key;
}

void finalizeBackendUnlocked() {
    // Both are safe no-ops when not initialized.
    LSFG_3_1P::finalize();
    LSFG_3_1::finalize();

    gState.prepared = false;
    gState.deviceKey = 0;
}

std::string prepareBackend(
        const std::string& dllPath,
        int multiplier,
        bool performance) {
    if (multiplier < 2 || multiplier > 8) {
        throw std::runtime_error(
                "Multiplier must be between 2x and 8x.");
    }

    auto shaders =
            extractFp32SpirvResources(dllPath);

    if (shaders.size() !=
        (kLastFp32Spirv - kFirstFp32Spirv + 1)) {
        throw std::runtime_error(
                "Incomplete FP32 SPIR-V shader set.");
    }

    // Keep the shader cache populated before LSFG asks the callback for
    // individual shader names.
    {
        std::lock_guard<std::mutex> lock(gMutex);
        finalizeBackendUnlocked();

        gState.shaders = std::move(shaders);
        gState.dllPath = dllPath;
        gState.multiplier = multiplier;
        gState.performance = performance;
    }

    const uint64_t deviceKey = probeDeviceKey();

    try {
        const auto loader =
                [](const std::string& name) {
                    return loadPreparedShader(name);
                };

        if (performance) {
            LSFG_3_1P::initialize(
                    deviceKey,
                    false,
                    1.0f,
                    static_cast<uint64_t>(multiplier - 1),
                    loader);
        } else {
            LSFG_3_1::initialize(
                    deviceKey,
                    false,
                    1.0f,
                    static_cast<uint64_t>(multiplier - 1),
                    loader);
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(gMutex);
        finalizeBackendUnlocked();
        gState.shaders.clear();
        throw;
    }

    {
        std::lock_guard<std::mutex> lock(gMutex);
        gState.prepared = true;
        gState.deviceKey = deviceKey;
    }

    std::ostringstream out;
    out << "LSFG backend ready\n"
        << "Shaders FP32 SPIR-V: 48/48\n"
        << "Multiplier: " << multiplier << "x\n"
        << "Mode: "
        << (performance ? "Performance (3.1P)" : "Quality (3.1)")
        << "\n"
        << "MediaProjection: not required by this backend";
    return out.str();
}

std::string backendStatus() {
    std::lock_guard<std::mutex> lock(gMutex);

    if (!gState.prepared) {
        return "LSFG backend not prepared.";
    }

    std::ostringstream out;
    out << "LSFG backend ready"
        << " | "
        << gState.multiplier << "x"
        << " | "
        << (gState.performance ? "3.1P" : "3.1")
        << " | shaders=" << gState.shaders.size();
    return out.str();
}

} // namespace

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativePrepareLosslessBackend(
        JNIEnv* env,
        jclass,
        jstring dllPath,
        jint multiplier,
        jboolean performance) {
    try {
        const std::string path =
                fromJString(env, dllPath);

        if (path.empty()) {
            return toJString(
                    env,
                    "LSFG backend error: DLL path is empty.");
        }

        return toJString(
                env,
                prepareBackend(
                        path,
                        static_cast<int>(multiplier),
                        performance == JNI_TRUE));
    } catch (const std::exception& error) {
        return toJString(
                env,
                std::string("LSFG backend error: ") +
                        error.what());
    } catch (...) {
        return toJString(
                env,
                "LSFG backend error: unknown native exception.");
    }
}

extern "C"
JNIEXPORT jstring JNICALL
Java_dev_framegen_android_MainActivity_nativeGetLosslessBackendStatus(
        JNIEnv* env,
        jclass) {
    return toJString(env, backendStatus());
}

extern "C"
JNIEXPORT void JNICALL
Java_dev_framegen_android_MainActivity_nativeReleaseLosslessBackend(
        JNIEnv*,
        jclass) {
    std::lock_guard<std::mutex> lock(gMutex);
    finalizeBackendUnlocked();
    gState.shaders.clear();
    gState.dllPath.clear();
}
