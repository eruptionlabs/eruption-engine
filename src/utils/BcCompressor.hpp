#pragma once

#include "utils/EtexLoader.hpp"

#include <cstdint>
#include <vector>

namespace eruption {

// Runtime block compression (BC1/BC3/BC5/BC7) of RGBA8 pixels into the same
// EtexData the offline baker (tools/bake_assets.py) produces, so both feed the
// exact same upload path (uploadEtexViaFrameCB / uploadPbrTextureSlot).
//
// Why runtime and not only offline: GLB maps carry their textures EMBEDDED in
// the container (parana_field: 164 PNGs, 137 Mpx). There is no file on disk to
// bake next to, and the engine must keep accepting a plain GLB with no cooked
// sidecar at all. So we compress on load and cache the result on disk keyed by
// the GLB's identity - the cache is a derived, regenerable artifact.
enum class BcKind {
    Albedo,  // BC1 (opaque) / BC3 (real alpha) - sRGB-ish colour, joint palette is fine
    Normal,  // BC5 - two independent channels, never BC1 (it would wreck the normal)
    Data,    // BC7 (or BC3 when BC7 is unavailable) - mrahw packs 4 independent channels
};

// True while the BC7 encoder tables are being used; init is idempotent and
// thread-safe (call once before the first parallel compress if you like).
void bcCompressorInit();

// What the GPU actually supports; set once at startup from
// vkGetPhysicalDeviceFormatProperties. Defaults to "everything off" so a caller
// that forgets to probe never produces an unsupported image.
struct BcFormatSupport {
    bool bc1 = false;
    bool bc3 = false;
    bool bc5 = false;
    bool bc7 = false;
    bool any() const { return bc1 || bc3 || bc5 || bc7; }
};
void setBcFormatSupport(const BcFormatSupport& s);
const BcFormatSupport& bcFormatSupport();

// Compress an RGBA8 image (tightly packed, 4 bytes per texel) into an EtexData
// with a full mip chain (area-average box filter, matching the engine's
// ImageUtils::downscaleRGBA semantics so BC and RGBA8 paths mip identically).
//
// Returns an invalid EtexData when the GPU cannot sample the needed format, or
// when the image is too small to be worth compressing - the caller then falls
// back to the plain RGBA8 path. This is the accessibility guarantee: no bake,
// no BC support, still renders.
// `roughnessBroadenR`, quando presente E kind==Data: aplica nitidez especular
// adaptativa a' la Toksvig ANTES de codificar cada nivel de mip - o canal G
// (roughness) de cada texel e' AUMENTADO proporcionalmente a quanto a normal
// map correspondente variou dentro da area que foi reduzida aquele texel
// (ver computeNormalMipVariance() abaixo). Vetor por nivel de mip, mesma
// contagem/dimensao que os niveis que este chamado vai gerar (mip0 primeiro).
// Sem isso (nullptr, o padrao), roughness so' e' reduzido/aumentado pelo
// autor - nunca pela microestrutura da propria normal map.
EtexData compressRgbaToEtex(const uint8_t* rgba, int width, int height, BcKind kind,
                            bool hasAlpha = false,
                            const std::vector<std::vector<float>>* roughnessBroadenR = nullptr);

// Variancia da normal map por nivel de mip (Toksvig 2005 / Neubelt & Pettineo,
// "Crafting a Next-Gen Material Pipeline for The Order: 1886", GDC 2013).
//
// `normalRgba` e' a normal map SINTETIZADA em espaco tangente, codificada
// [0,255] representando [-1,1] por canal (mesmo layout que vai pra
// compressRgbaToEtex(..., BcKind::Normal, ...)).
//
// Devolve, para cada nivel de mip (0 = resolucao total, ate' 1x1), um mapa
// `r` do MESMO tamanho daquele nivel: r=1 quando as normais mais finas que
// caem dentro deste texel apontavam todas pro mesmo lado (nada de
// microestrutura escondida); r->0 quando apontavam em direcoes bem
// diferentes (microestrutura sub-texel que a normal map deveria estar
// mostrando, mas nao pode, porque o texel so' guarda UMA direcao media).
//
// Essa perda de energia direcional E' A MESMA MATEMATICA que causa
// cintilacao especular quando se ve a textura de longe (o motivo do
// specular AA em tela, pbr_common.glsl, existir) - so' que aqui e' medida
// UMA VEZ NO BAKE, por mip, em vez de estimada por pixel toda vez que a
// camera se move. mip0 e' sempre 1.0 em todo texel (nada foi medido ainda -
// e' a fonte, nao uma reducao dela).
std::vector<std::vector<float>> computeNormalMipVariance(const uint8_t* normalRgba, int width, int height);

// Serialize / parse an EtexData into the on-disk .etex byte layout (same
// header the offline baker writes), used by the GLB texture cache bundle.
void appendEtexBytes(const EtexData& e, std::vector<uint8_t>& out);
bool readEtexBytes(const uint8_t* data, size_t size, size_t& cursor, EtexData& out);

} // namespace eruption
