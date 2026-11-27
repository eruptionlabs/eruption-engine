#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"
#include "utils/VkTrace.hpp"

#include <atomic>

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
    s_lastLoaded = filepath;
    if (s_noBindless && filepath.size() > kExt.size() &&
        filepath.compare(filepath.size() - kExt.size(), kExt.size(), kExt) == 0) {
        auto nb = loadSPIRVExact(filepath.substr(0, filepath.size() - kExt.size()) + ".nb.spv", true);
        if (!nb.empty()) return nb;
    }
#if defined(__EMSCRIPTEN__)
    // Navegador: o Vulkan e' o dispositivo nulo (platform/web/VulkanNull.cpp)
    // e os .spv nem vao no pacote. Um cabecalho SPIR-V basta para o renderer
    // seguir; o shader de verdade e' o GLSL ES do caminho WebGL2.
    return {0x07230203u, 0x00010000u, 0u, 1u, 0u};
#else
    return loadSPIRVExact(filepath, false);
#endif
}

std::vector<uint32_t> ShaderCompiler::loadSPIRVExact(const std::string& filepath, bool quiet) {
    ERUPTION_BOOT_SCOPE(SpvFile);
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

    // Quase todo shader vem do mesmo prefixo ("shaders/"): tenta primeiro o que deu certo da ultima vez. Antes
    // cada shader gastava 4 a 5 aberturas que falhavam (build/..., build/shaders/...) ate' achar o arquivo, e
    // com o .nb.spv ausente (ele e' tentado antes) as 15 falhavam: centenas de chamadas ao sistema no init.
    static std::atomic<int> s_lastHit{-1};
    std::ifstream file;
    std::string tryPath;
    const int hint = s_lastHit.load(std::memory_order_relaxed);
    if (hint >= 0 && hint < static_cast<int>(tryPaths.size())) {
        file = std::ifstream(tryPaths[hint], std::ios::ate | std::ios::binary);
        if (file.is_open()) tryPath = tryPaths[hint];
    }
    if (!file.is_open()) {
        for (size_t i = 0; i < tryPaths.size(); ++i) {
            file = std::ifstream(tryPaths[i], std::ios::ate | std::ios::binary);
            if (file.is_open()) {
                tryPath = tryPaths[i];
                s_lastHit.store(static_cast<int>(i), std::memory_order_relaxed);
                break;
            }
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
