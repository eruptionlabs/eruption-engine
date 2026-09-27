#include "utils/GpuAutoSelect.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <linux/limits.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace eruption {

namespace {

std::string vendorString(uint32_t vendorID) {
    switch (vendorID) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x1022: return "AMD";
        case 0x8086: return "Intel";
        case 0x5143: return "Qualcomm";
        case 0x13B5: return "ARM";
        case 0x1010: return "ImgTec";
        default: return "Unknown";
    }
}

} // anonymous namespace

std::vector<GpuInfo> GpuAutoSelect::enumerateGpus() {
    std::vector<GpuInfo> gpus;

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Eruption Engine GPU Probe";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = glfwExtensionCount;
    createInfo.ppEnabledExtensionNames = glfwExtensions;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        ERUPTION_LOG_WARN("[GpuAutoSelect] Failed to create Vulkan probe instance: %d", result);
        return gpus;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount > 0) {
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        for (uint32_t i = 0; i < deviceCount; ++i) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devices[i], &props);

            GpuInfo info;
            info.index = static_cast<int>(i);
            info.name = props.deviceName;
            info.vendorID = props.vendorID;
            info.deviceID = props.deviceID;

            switch (props.deviceType) {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   info.deviceType = "PHYSICAL_DEVICE_TYPE_DISCRETE_GPU"; break;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: info.deviceType = "PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU"; break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    info.deviceType = "PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU"; break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:            info.deviceType = "PHYSICAL_DEVICE_TYPE_CPU"; break;
                default:                                     info.deviceType = "PHYSICAL_DEVICE_TYPE_OTHER"; break;
            }

            // Try to get driver properties if VK_KHR_driver_properties is available.
            VkPhysicalDeviceDriverPropertiesKHR driverProps{};
            driverProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR;
            VkPhysicalDeviceProperties2 props2{};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &driverProps;
            vkGetPhysicalDeviceProperties2(devices[i], &props2);
            info.driverName = driverProps.driverName;

            gpus.push_back(info);
        }
    }

    vkDestroyInstance(instance, nullptr);
    return gpus;
}

int GpuAutoSelect::deviceTypeRank(const std::string& type) {
    if (type == "PHYSICAL_DEVICE_TYPE_DISCRETE_GPU") return 4;
    if (type == "PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU") return 3;
    if (type == "PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU") return 2;
    if (type == "PHYSICAL_DEVICE_TYPE_CPU") return 1;
    return 0;
}

int GpuAutoSelect::pickBestGpuIndex(const std::vector<GpuInfo>& gpus) {
    if (gpus.empty()) return -1;

    int bestIdx = 0;
    int bestRank = deviceTypeRank(gpus[0].deviceType);
    for (size_t i = 1; i < gpus.size(); ++i) {
        int rank = deviceTypeRank(gpus[i].deviceType);
        if (rank > bestRank) {
            bestRank = rank;
            bestIdx = static_cast<int>(i);
        }
    }
    return bestIdx;
}

std::string GpuAutoSelect::getExecutablePath() {
    char path[PATH_MAX + 1]{};
    ssize_t len = readlink("/proc/self/exe", path, PATH_MAX);
    if (len <= 0) return {};
    return std::string(path, static_cast<size_t>(len));
}

std::string GpuAutoSelect::getConfigPath() {
    std::string exe = getExecutablePath();
    if (exe.empty()) return {};
    size_t pos = exe.find_last_of('/');
    if (pos == std::string::npos) return {};
    return exe.substr(0, pos) + "/data/launcher_config.json";
}

bool GpuAutoSelect::readConfig(std::string& outVendor, std::string& outIndex) {
    std::string path = getConfigPath();
    if (path.empty()) return false;

    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        size_t keyPos = line.find('"');
        if (keyPos == std::string::npos) continue;
        size_t keyEnd = line.find('"', keyPos + 1);
        if (keyEnd == std::string::npos) continue;
        std::string key = line.substr(keyPos + 1, keyEnd - keyPos - 1);

        size_t valPos = line.find('"', keyEnd + 1);
        if (valPos == std::string::npos) continue;
        size_t valEnd = line.find('"', valPos + 1);
        if (valEnd == std::string::npos) continue;
        std::string val = line.substr(valPos + 1, valEnd - valPos - 1);

        if (key == "gpu_vendor") outVendor = val;
        else if (key == "gpu_index") outIndex = val;
    }
    return true;
}

void GpuAutoSelect::writeConfig(const std::string& vendor, const std::string& index) {
    std::string path = getConfigPath();
    if (path.empty()) return;

    std::ofstream f(path);
    if (!f.is_open()) return;

    f << "{\n";
    f << "    \"gpu_vendor\": \"" << vendor << "\",\n";
    f << "    \"gpu_index\": \"" << index << "\"\n";
    f << "}\n";
}

bool GpuAutoSelect::ensureBestGpu() {
    // Environment overrides always win and skip all logic.
    const char* envVendor = std::getenv("ERUPTION_GPU_VENDOR");
    const char* envIndex = std::getenv("ERUPTION_GPU_INDEX");
    const char* envDisable = std::getenv("ERUPTION_DISABLE_GPU_SELECT");
    if (envDisable) {
        return true;
    }

    std::string cfgVendor, cfgIndex;
    readConfig(cfgVendor, cfgIndex);

    if (envVendor) cfgVendor = envVendor;
    if (envIndex) cfgIndex = envIndex;

    // Auto-detect if nothing configured.
    bool autoDetected = false;
    if (cfgVendor.empty() && cfgIndex.empty()) {
        auto gpus = enumerateGpus();
        if (gpus.empty()) {
            ERUPTION_LOG_WARN("[GpuAutoSelect] Could not enumerate Vulkan GPUs; using loader default");
            return true;
        }

        ERUPTION_LOG_WARN("[GpuAutoSelect] Available GPUs:");
        for (const auto& gpu : gpus) {
            ERUPTION_LOG_WARN("  [%d] %s (vendor: %s, type: %s, driver: %s)",
                            gpu.index, gpu.name.c_str(),
                            vendorString(gpu.vendorID).c_str(),
                            gpu.deviceType.c_str(), gpu.driverName.c_str());
        }

        // Hybrid NVIDIA+Intel laptops on Linux/X11 need PRIME render offload
        // enabled before Vulkan device enumeration, otherwise vkQueuePresentKHR
        // stalls on the cross-GPU copy and FPS collapses to ~2.
        bool hasNvidia = false, hasIntel = false;
        for (const auto& gpu : gpus) {
            if (gpu.vendorID == 0x10DE) hasNvidia = true;
            if (gpu.vendorID == 0x8086 && gpu.deviceType == "PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU") hasIntel = true;
        }
        if (hasNvidia && hasIntel &&
            std::getenv("__NV_PRIME_RENDER_OFFLOAD") == nullptr &&
            std::getenv("__VK_LAYER_NV_optimus") == nullptr) {
            ERUPTION_LOG_WARN("[GpuAutoSelect] Hybrid NVIDIA+Intel detected; enabling PRIME render offload");
            setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
            setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 1);
            // Re-enumerate so the device order reflects the offload layer and
            // the auto-selected index points at the correct physical device.
            gpus = enumerateGpus();
        }

        int best = pickBestGpuIndex(gpus);
        if (best < 0) best = 0;
        cfgIndex = std::to_string(gpus[best].index);
        cfgVendor = vendorString(gpus[best].vendorID);

        ERUPTION_LOG_WARN("[GpuAutoSelect] Auto-selected GPU index %s (%s)",
                        cfgIndex.c_str(), gpus[best].name.c_str());
        autoDetected = true;
    }

    // Make sure ERUPTION_GPU_INDEX is set so VulkanContext uses the right device.
    if (!cfgIndex.empty() && std::getenv("ERUPTION_GPU_INDEX") == nullptr) {
        setenv("ERUPTION_GPU_INDEX", cfgIndex.c_str(), 1);
    }

    if (autoDetected) {
        writeConfig(cfgVendor, cfgIndex);
    }

    return true;
}

} // namespace eruption
