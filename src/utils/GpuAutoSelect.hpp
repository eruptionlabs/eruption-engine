#pragma once

#include <string>
#include <vector>

namespace eruption {

struct GpuInfo {
    int index = -1;
    std::string name;
    std::string deviceType;
    std::string driverName;
    std::string driverInfo;
    uint32_t vendorID = 0;
    uint32_t deviceID = 0;
};

class GpuAutoSelect {
public:
    // Detects available Vulkan GPUs and, if necessary, restarts the current
    // process with environment variables that force the best driver/GPU.
    // Should be called before any Vulkan initialization.
    // Returns true if the caller should proceed, false if the process was
    // restarted and this instance should exit.
    static bool ensureBestGpu();

    // Returns the list of GPUs found on the system.
    static std::vector<GpuInfo> enumerateGpus();

    // Picks the best GPU index from a list (discrete > integrated > virtual > cpu).
    static int pickBestGpuIndex(const std::vector<GpuInfo>& gpus);

private:
    static bool readConfig(std::string& outVendor, std::string& outIndex);
    static void writeConfig(const std::string& vendor, const std::string& index);
    static std::string getExecutablePath();
    static std::string getConfigPath();
    static bool parseVulkanInfo(std::vector<GpuInfo>& out);
    static int deviceTypeRank(const std::string& type);
};

} // namespace eruption
