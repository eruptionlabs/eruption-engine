#pragma once

#include <vector>
#include <string>
#include <cstdint>

namespace eruption {

enum class ShaderStage {
    Vertex,
    Fragment,
    Compute,
    Geometry,
    TessControl,
    TessEvaluation
};

class ShaderCompiler {
public:
    static std::vector<uint32_t> compileGLSL(const std::string& source,
                                              ShaderStage stage,
                                              const std::string& entryPoint = "main");

    static std::vector<uint32_t> loadSPIRV(const std::string& filepath);

    static bool compileOrLoad(const std::string& glslPath,
                               const std::string& spvPath,
                               ShaderStage stage,
                               std::vector<uint32_t>& outCode);
};

} // namespace eruption
