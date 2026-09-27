#pragma once

#include <vulkan/vulkan.h>
#include <cstdlib>
#include <cstring>

namespace eruption {

// Formato dos alvos INTERMEDIARIOS de pos-processamento.
//
// Por que isto existe: o bloco de post e' limitado por BANDA, nao por ALU.
// Medido nesta engine, o custo escala 5,4x quando a resolucao sobe 4x (3,73 ms
// a 2,07 Mpx contra 0,69 ms a 0,52 Mpx) - assinatura de banda. Sao ~6 alvos de
// tela cheia; em RGBA16F cada um custa 8 bytes por pixel (16,6 MB a 1920x1080).
//
// B10G11R11_UFLOAT_PACK32 sao 4 bytes por pixel: METADE da banda, de graca.
// Cabe porque (a) nenhum shader da cadeia de post le o proprio alfa de volta -
// a unica leitura de alfa vem de cloudVolumeCoverageTexture, que e' de outro
// passe - e (b) nao existe blend por DST_ALPHA em lugar nenhum. Valores HDR de
// post sao nao-negativos, entao o formato UNSIGNED serve.
//
// Isto importa MUITO mais na 930M que nesta maquina de desenvolvimento: Maxwell
// GM108 tem ~14 GB/s de banda contra ~256 GB/s da 4060. E' o final boss.
//
// COMPATIBILIDADE (regra da engine: formatos internos sao so otimizacao, e tudo
// tem que ter caminho de volta):
//   - o suporte e' CONSULTADO no device; sem COLOR_ATTACHMENT_BLEND o codigo
//     cai sozinho para RGBA16F, sem quebrar;
//   - ERUPTION_POST_FORMAT=rgba16f forca o formato antigo, =packed forca o
//     novo (A/B e escape, independente do preset).
// Preferencia vinda do PRESET (data/graphics.json: "post_format"). A troca e'
// banda por precisao, entao quem decide e' o preset, nao a engine inteira:
// low/medium ganham a banda (e' o caminho da 930M), high fica em RGBA16F para
// nao perder gradiente de ceu. Tem que ser chamado ANTES do PostProcessor::init.
inline bool& postPackedPreference() { static bool pref = false; return pref; }

inline VkFormat postColorFormat(VkPhysicalDevice gpu) {
    static VkFormat cached = VK_FORMAT_UNDEFINED;
    if (cached != VK_FORMAT_UNDEFINED) return cached;

    const char* env = std::getenv("ERUPTION_POST_FORMAT");
    if (env && std::strcmp(env, "packed") == 0) postPackedPreference() = true;
    if (env && std::strcmp(env, "rgba16f") == 0) {
        cached = VK_FORMAT_R16G16B16A16_SFLOAT;
        return cached;
    }

    cached = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (gpu != VK_NULL_HANDLE && postPackedPreference()) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &props);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((props.optimalTilingFeatures & need) == need) {
            cached = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        }
    }
    return cached;
}

} // namespace eruption
