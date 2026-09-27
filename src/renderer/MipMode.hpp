#pragma once

#include <cstdint>

namespace eruption {

enum class MipGenerationMode : uint8_t {
    BLIT_LINEAR = 0,        // Modo 0: vkCmdBlitImage linear (padrão, funciona em todos os assets)
    DIRECTXTEX_OFFLINE = 1, // Modo 1: BC7/KTX2 pré-gerado offline (fallback para blit se não existir)
    TOKSVIG_PBR = 2,        // Modo 2: Toksvig compute (requer PBR ativo — não implementado ainda)
    COMPUTE_SHARED_LDS = 3, // Modo 3: GPU Compute Shader com Shared Memory (LDS) e correções
    COUNT
};

struct MipConfig {
    MipGenerationMode mode = MipGenerationMode::COMPUTE_SHARED_LDS;
    bool forceRegenOnSwitch = false;
    bool showWarningNoMipmaps = true;
    bool enabled = true; // Se false, todas as texturas usam mipLevels = 1
};

inline const char* mipModeName(MipGenerationMode mode) {
    switch (mode) {
        case MipGenerationMode::BLIT_LINEAR: return "Blit Linear (Runtime)";
        case MipGenerationMode::DIRECTXTEX_OFFLINE: return "DirectXTex Offline (Cache)";
        case MipGenerationMode::TOKSVIG_PBR: return "Toksvig PBR (Normal+Roughness)";
        case MipGenerationMode::COMPUTE_SHARED_LDS: return "GPU Compute Shared Memory (LDS)";
        default: return "Unknown";
    }
}

inline const char* mipModeDescription(MipGenerationMode mode) {
    switch (mode) {
        case MipGenerationMode::BLIT_LINEAR:
            return "Geração runtime via vkCmdBlitImage com filtro linear. Fallback universal.";
        case MipGenerationMode::DIRECTXTEX_OFFLINE:
            return "Carrega mipmaps pré-gerados do cache KTX2. Requer pipeline de build de assets.";
        case MipGenerationMode::TOKSVIG_PBR:
            return "Toksvig para normal maps + roughness ajustado. Requer sistema PBR ativo.";
        case MipGenerationMode::COMPUTE_SHARED_LDS:
            return "Geração via Compute Shader otimizada (LDS / Memória Compartilhada) com correção de Gamma e preservação de Alpha.";
        default: return "";
    }
}

} // namespace eruption
