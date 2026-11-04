#pragma once

#include <vulkan/vulkan.h>

// FORMATOS ENXUTOS (spec 50, "Fluxo Frio"): alvos que podem guardar menos
// bytes por texel sem mudar o que se ve'. Cada um e' uma chave de preset
// (data/graphics.json) com escape por variavel de ambiente para A/B na
// bancada, e todos entram DESLIGADOS: so' viram padrao com PSNR/SSIM/LPIPS
// provados contra o formato antigo. A decisao vale uma vez, antes de qualquer
// pipeline existir (Engine::init), e a GPU e' consultada: sem suporte, volta
// ao formato antigo sem quebrar.
namespace eruption::lean {

// Atlas de sombra em D16_UNORM em vez de D32_SFLOAT: metade dos bytes na
// gravacao das cascatas e em cada amostra do teste de sombra. Preset
// "shadow_depth16"; ERUPTION_TEST_SHADOW_D16=1|0 forca.
inline bool& shadowD16() {
    static bool on = false;
    return on;
}

inline VkFormat shadowDepthFormat() { return shadowD16() ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT; }

// O atlas e' anexo de profundidade, amostrado (teste de sombra, decalque,
// raios de sol) e destino/origem de copia (sombra pre-assada).
inline bool shadowD16Supported(VkPhysicalDevice gpu) {
    if (gpu == VK_NULL_HANDLE) return false;
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_D16_UNORM, &p);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (p.optimalTilingFeatures & need) == need;
}

// OPERACOES ENXUTAS (spec 52). Mesmo esquema: chave de preset + env de A/B,
// decididas uma vez no Engine::init, antes de qualquer pipeline.

// OE-01: load/store enxutos (cascata guardada sem LOAD do atlas, profundidade
// do ceu-para-textura descartada, passe vazio nao abre, STORE_OP_NONE nativo,
// mascara reativa so' gravada quando o FSR le'). Preset "lean_ops";
// ERUPTION_TEST_LEAN_OPS=1|0 forca. A imagem nao muda: so' deixa de ler e
// gravar o que ninguem usa.
inline bool& leanOps() {
    static bool on = false;
    return on;
}

// OE-02: o composite do DoF vai para dentro do Composite (um alvo de tela
// cheia a menos). Preset "post_fused"; ERUPTION_TEST_POST_FUSED=1|0 forca.
inline bool& postFused() {
    static bool on = false;
    return on;
}

// OE-03: stream de vertice so' da sombra (posicao 12 B + UV/indice 12 B por
// vertice) no lugar do vertice inteiro de 88 B. Preset "shadow_vertex_stream";
// ERUPTION_TEST_SHADOW_STREAM=1|0 forca.
inline bool& shadowStream() {
    static bool on = false;
    return on;
}

} // namespace eruption::lean
