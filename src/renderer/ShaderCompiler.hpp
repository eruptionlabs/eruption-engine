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
    // Sem descriptor indexing: loadSPIRV prefere a variante <nome>.nb.spv
    // (array de texturas fixo), quando o shader tem uma.
    static void setNoBindless(bool on) { s_noBindless = on; }
    // Verdadeiro quando os shaders carregados sao os .nb (sem descriptor indexing).
    static bool noBindless() { return s_noBindless; }
    // Último .spv lido nesta thread: o log de pipeline recusada diz qual era
    // (o driver só devolve o código de erro).
    static const std::string& lastLoaded() { return s_lastLoaded; }

    static bool compileOrLoad(const std::string& glslPath,
                               const std::string& spvPath,
                               ShaderStage stage,
                               std::vector<uint32_t>& outCode);

private:
    static inline bool s_noBindless = false;
    static inline thread_local std::string s_lastLoaded;
    static std::vector<uint32_t> loadSPIRVExact(const std::string& filepath, bool quiet);
};

} // namespace eruption
