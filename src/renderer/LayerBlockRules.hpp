#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

// Regras puras do array de camadas em bloco (TextureLayers, spec 51 TC-05):
// o que vai para o A em bloco e quando a camada pode ser copia de bloco direta
// da fonte. Sem Vulkan alem do enum, para o teste (tests/test_texture_codec.cpp).
namespace eruption::layerrules {

inline bool isBlockFormat(VkFormat f) {
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) ||
           (f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK) ||
           (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}

inline bool isSrgb(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        return true;
    default:
        // ASTC: cada tamanho de bloco vem em par UNORM/SRGB, o SRGB depois.
        return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK &&
               (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) % 2 == 1;
    }
}

// A textura vai para o A em bloco: ja' chega comprimida (a perda ja' foi
// aceita) ou e' cor sRGB. Dados lineares sem compressao (RGBA8/R8/RG8) e as sem
// filtro (pixel art, sprites) ficam como chegaram, no B em RGBA8.
inline bool wantsBlock(VkFormat source, bool nearest) {
    return !nearest && (isBlockFormat(source) || isSrgb(source));
}

// UNORM e sRGB do mesmo bloco tem os mesmos bits (o shader decodifica o sRGB
// pela tabela, como no RGBA8): a copia de bloco entre os dois vale.
inline VkFormat blockClass(VkFormat f) {
    switch (f) {
    case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    default: return f;
    }
}

// O que se sabe da fonte (imagem + view) para decidir a copia direta.
struct Source {
    VkFormat imageFormat = VK_FORMAT_UNDEFINED, viewFormat = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, mips = 1;
    uint32_t baseMip = 0, levelCount = 1, baseLayer = 0; // da view (levelCount pode ser VK_REMAINING_MIP_LEVELS)
    bool transferSrc = false, view2D = true, singleSample = true, optimal = true;
};

// Fonte quadrada em potencia de 2, lado >= camada e mips ate' 1x1 a partir
// dela; `mip` = mip da fonte que vira o mip 0 da camada.
inline bool sourceSizeFits(const Source& s, uint32_t layerSize, uint32_t layerMips, uint32_t& mip) {
    if (!s.transferSrc || !s.view2D || !s.singleSample || !s.optimal || s.baseMip >= s.mips) return false;
    const uint32_t w = s.width >> s.baseMip, h = s.height >> s.baseMip;
    if (w == 0 || w != h || w < layerSize || (w & (w - 1)) != 0) return false;
    uint32_t skip = 0;
    while ((w >> skip) > layerSize) ++skip;
    const uint32_t viewLevels = s.levelCount == VK_REMAINING_MIP_LEVELS ? s.mips - s.baseMip : s.levelCount;
    if (skip + layerMips > viewLevels || s.baseMip + skip + layerMips > s.mips) return false;
    mip = s.baseMip + skip;
    return true;
}

// A camada pode ser copia de bloco da fonte: mesma classe de bloco do A e
// sourceSizeFits.
inline bool directCopy(const Source& s, VkFormat blockFormat, uint32_t layerSize, uint32_t layerMips, uint32_t& mip) {
    if (blockFormat == VK_FORMAT_UNDEFINED) return false;
    if (blockClass(s.imageFormat) != blockFormat || blockClass(s.viewFormat) != blockFormat) return false;
    return sourceSizeFits(s, layerSize, layerMips, mip);
}

// Fonte ETC2 RGB (albedo opaco da familia ETC2) num A em ETC2 RGBA: o bloco
// RGBA e' um bloco EAC de alfa seguido do bloco ETC2 RGB, entao a camada sai
// dos blocos da fonte com um alfa constante 255 na frente - sem perda.
inline bool etc2AddAlpha(const Source& s, VkFormat blockFormat, uint32_t layerSize, uint32_t layerMips,
                         uint32_t& mip) {
    auto rgb = [](VkFormat f) { return f == VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK || f == VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK; };
    if (blockFormat != VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK || !rgb(s.imageFormat) || !rgb(s.viewFormat)) return false;
    return sourceSizeFits(s, layerSize, layerMips, mip);
}

// Bloco EAC (8 bytes, big-endian) de alfa 255 em todo texel: base 255,
// multiplicador 1, tabela 0, indice 4 (+2) em todos -> 257, saturado em 255.
constexpr uint8_t kEacAlpha255[8] = {0xFF, 0x10, 0x92, 0x49, 0x24, 0x92, 0x49, 0x24};

// `blocks` blocos ETC2 RGB (8 B) -> ETC2 RGBA (16 B: alfa EAC, depois o RGB).
inline void etc2RgbToRgba(const uint8_t* rgb, size_t blocks, uint8_t* rgba) {
    for (size_t b = 0; b < blocks; ++b) {
        for (int i = 0; i < 8; ++i) rgba[b * 16 + size_t(i)] = kEacAlpha255[i];
        for (int i = 0; i < 8; ++i) rgba[b * 16 + 8 + size_t(i)] = rgb[b * 8 + size_t(i)];
    }
}

} // namespace eruption::layerrules
