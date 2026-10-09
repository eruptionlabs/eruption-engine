#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace eruption {

std::vector<uint32_t> ShaderCompiler::compileGLSL(const std::string& source,
                                                   ShaderStage stage,
                                                   const std::string& entryPoint) {
    (void)source;
    (void)stage;
    (void)entryPoint;
    ERUPTION_LOG_WARN("Runtime GLSL compilation not implemented — compile shaders at build time");
    return {};
}

std::vector<uint32_t> ShaderCompiler::loadSPIRV(const std::string& filepath) {
    static const std::string kExt = ".spv";
    if (s_noBindless && filepath.size() > kExt.size() &&
        filepath.compare(filepath.size() - kExt.size(), kExt.size(), kExt) == 0) {
        auto nb = loadSPIRVExact(filepath.substr(0, filepath.size() - kExt.size()) + ".nb.spv", true);
        if (!nb.empty()) return nb;
    }
    return loadSPIRVExact(filepath, false);
}

std::vector<uint32_t> ShaderCompiler::loadSPIRVExact(const std::string& filepath, bool quiet) {
    // Try multiple paths: build/shaders/<subdir>/<file>, build/shaders/<file>, build/<filepath>, <filepath>
    size_t lastSlash = filepath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;
    std::string subdir = "";
    if (lastSlash != std::string::npos) {
        size_t firstSlash = filepath.find('/');
        if (firstSlash != std::string::npos && firstSlash < lastSlash) {
            subdir = filepath.substr(0, lastSlash + 1);
        }
    }

    std::vector<std::string> tryPaths = {
        "build/" + filepath,
        "build/shaders/" + filepath,
        "build/shaders/" + subdir + filename,
        "build/shaders/" + filename,
        "shaders/" + filepath,
        "shaders/" + subdir + filename,
        "shaders/" + filename,
        "../build/" + filepath,
        "../build/shaders/" + filepath,
        "../build/shaders/" + subdir + filename,
        "../build/shaders/" + filename,
        "../shaders/" + filepath,
        "../shaders/" + subdir + filename,
        "../shaders/" + filename,
        filepath
    };

    std::ifstream file;
    std::string tryPath;
    for (const auto& p : tryPaths) {
        file = std::ifstream(p, std::ios::ate | std::ios::binary);
        if (file.is_open()) {
            tryPath = p;
            break;
        }
    }
    if (!file.is_open()) {
        if (quiet) return {};
        ERUPTION_LOG_ERROR("Failed to open SPIR-V file: %s", filepath.c_str());
        return {};
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    ERUPTION_LOG_INFO("Loaded SPIR-V: %s (%zu bytes)", tryPath.c_str(), fileSize);
    if (fileSize % sizeof(uint32_t) != 0) {
        ERUPTION_LOG_ERROR("SPIR-V file size is not a multiple of 4: %s", filepath.c_str());
        return {};
    }

    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
    file.close();

    return buffer;
}

bool ShaderCompiler::compileOrLoad(const std::string& glslPath,
                                    const std::string& spvPath,
                                    ShaderStage stage,
                                    std::vector<uint32_t>& outCode) {
    (void)glslPath;
    (void)stage;
    outCode = loadSPIRV(spvPath);
    return !outCode.empty();
}

} // namespace eruption
