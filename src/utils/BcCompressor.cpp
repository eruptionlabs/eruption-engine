#include "utils/BcCompressor.hpp"

#include "core/JobSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include <vulkan/vulkan.h>

#ifdef ERUPTION_HAVE_BC_ENCODER
// Headers de terceiro: struct anonima dispara -Wpedantic que nao e' nosso.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "rgbcx.h"
#include "bc7enc.h"
#pragma GCC diagnostic pop
#endif

namespace eruption {

namespace {

BcFormatSupport g_support;
std::once_flag g_initOnce;

constexpr uint32_t kEtexMagic = 0x31585445; // 'ETX1'

// Area-average halving. Same filter as ImageUtils::downscaleRGBA (fixed in
// commit "area-average downscale"): a corner-tap box would shift the mip half
// a texel and BC would inherit that shift, making the A/B look like a codec
// regression when it is a filter bug.
[[maybe_unused]] void halveRgba(const std::vector<uint8_t>& src, int sw, int sh,
               std::vector<uint8_t>& dst, int& dw, int& dh) {
    dw = std::max(1, sw / 2);
    dh = std::max(1, sh / 2);
    dst.resize(static_cast<size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const int y0 = std::min(y * 2, sh - 1);
        const int y1 = std::min(y * 2 + 1, sh - 1);
        for (int x = 0; x < dw; ++x) {
            const int x0 = std::min(x * 2, sw - 1);
            const int x1 = std::min(x * 2 + 1, sw - 1);
            const uint8_t* a = &src[(static_cast<size_t>(y0) * sw + x0) * 4];
            const uint8_t* b = &src[(static_cast<size_t>(y0) * sw + x1) * 4];
            const uint8_t* c = &src[(static_cast<size_t>(y1) * sw + x0) * 4];
            const uint8_t* d = &src[(static_cast<size_t>(y1) * sw + x1) * 4];
            uint8_t* o = &dst[(static_cast<size_t>(y) * dw + x) * 4];
            for (int ch = 0; ch < 4; ++ch) {
                o[ch] = static_cast<uint8_t>((a[ch] + b[ch] + c[ch] + d[ch] + 2) / 4);
            }
        }
    }
}

// Gather one 4x4 block, clamping at the edges so mips smaller than a block
// (2x2, 1x1) still produce a well-defined block instead of reading garbage.
inline void gatherBlock(const std::vector<uint8_t>& img, int w, int h,
                        int bx, int by, uint8_t out[64]) {
    for (int y = 0; y < 4; ++y) {
        const int sy = std::min(by + y, h - 1);
        for (int x = 0; x < 4; ++x) {
            const int sx = std::min(bx + x, w - 1);
            std::memcpy(&out[(y * 4 + x) * 4], &img[(static_cast<size_t>(sy) * w + sx) * 4], 4);
        }
    }
}

[[maybe_unused]] size_t blockBytesFor(VkFormat f) {
    switch (f) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return 8;
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK: return 16;
        default: return 0;
    }
}

#ifdef ERUPTION_HAVE_BC_ENCODER
void encodeLevel(VkFormat fmt, const std::vector<uint8_t>& img, int w, int h,
                 std::vector<uint8_t>& out) {
    const int bw = (w + 3) / 4;
    const int bh = (h + 3) / 4;
    const size_t bb = blockBytesFor(fmt);
    out.resize(static_cast<size_t>(bw) * bh * bb);

    bc7enc_compress_block_params bc7p{};
    if (fmt == VK_FORMAT_BC7_UNORM_BLOCK) {
        bc7enc_compress_block_params_init(&bc7p);
        // mrahw is DATA (metallic/roughness/height/wetness), not colour: the
        // perceptual luma weighting would spend bits on the wrong channels.
        bc7enc_compress_block_params_init_linear_weights(&bc7p);
        // Runtime budget: partitions dominate the cost. 16 keeps most of the
        // quality that made BC7 necessary here (BC3 on mrahw measured RMS 9-14)
        // at a fraction of the time of the default 64.
        bc7p.m_max_partitions = 16;
        bc7p.m_uber_level = 0;
    }

    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            uint8_t block[64];
            gatherBlock(img, w, h, bx * 4, by * 4, block);
            uint8_t* dst = &out[(static_cast<size_t>(by) * bw + bx) * bb];
            switch (fmt) {
                case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
                    rgbcx::encode_bc1(5, dst, block, true, false);
                    break;
                case VK_FORMAT_BC3_UNORM_BLOCK:
                    rgbcx::encode_bc3(5, dst, block);
                    break;
                case VK_FORMAT_BC5_UNORM_BLOCK:
                    // R,G of the tangent-space normal; Z is reconstructed in the
                    // shader from x/y (BC5 stores exactly two channels).
                    rgbcx::encode_bc5(dst, block, 0, 1, 4);
                    break;
                case VK_FORMAT_BC7_UNORM_BLOCK:
                    bc7enc_compress_block(dst, block, &bc7p);
                    break;
                default:
                    break;
            }
        }
    }
}
#endif // ERUPTION_HAVE_BC_ENCODER

// Toksvig factor (Neubelt & Pettineo, GDC 2013): dado `r` (comprimento do
// vetor normal medio, pre-renormalizacao - ver computeNormalMipVariance) e a
// roughness AUTORAL do texel, devolve a roughness EFETIVA que precisa ser
// usada pra' o lobulo especular parecer certo depois que a microestrutura da
// normal foi perdida no mip. r=1 -> f=1 -> roughness inalterada. r->0 ->
// f->0 -> roughness -> 1 (superficie lida como totalmente difusa/rugosa).
inline float toksvigRoughness(float baseRoughness, float r) {
    r = std::clamp(r, 1e-4f, 1.0f);
    const float f = r / (r + baseRoughness * (1.0f - r));
    return std::clamp(baseRoughness / std::max(f, 1e-4f), 0.0f, 1.0f);
}

} // namespace

std::vector<std::vector<float>> computeNormalMipVariance(const uint8_t* normalRgba, int width, int height) {
    std::vector<std::vector<float>> result;
    if (!normalRgba || width < 1 || height < 1) return result;

    int w = width, h = height;
    const size_t n0 = static_cast<size_t>(w) * h;
    std::vector<float> curX(n0), curY(n0), curZ(n0);
    for (size_t i = 0; i < n0; ++i) {
        curX[i] = normalRgba[i * 4 + 0] / 255.0f * 2.0f - 1.0f;
        curY[i] = normalRgba[i * 4 + 1] / 255.0f * 2.0f - 1.0f;
        curZ[i] = normalRgba[i * 4 + 2] / 255.0f * 2.0f - 1.0f;
    }
    // Mip0: cada texel E' uma amostra so', nao ha' o que ter variado ainda.
    result.emplace_back(n0, 1.0f);

    // MESMA cascata de halveRgba (clamp nas bordas, quadrante 2x2), so' que
    // em vetor UNITARIO float em vez de byte, e SEM renormalizar entre
    // niveis - a normal map de verdade (halveRgba) tambem nao renormaliza:
    // e' o proprio encolhimento acumulado do byte medio, nivel apos nivel,
    // que da' o sinal de variancia certo pra bater com o que a GPU realmente
    // amostra em cada mip.
    while (!(w == 1 && h == 1)) {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        const size_t nCount = static_cast<size_t>(nw) * nh;
        std::vector<float> nx(nCount), ny(nCount), nz(nCount), r(nCount);
        for (int y = 0; y < nh; ++y) {
            const int y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
            for (int x = 0; x < nw; ++x) {
                const int x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                const size_t i00 = static_cast<size_t>(y0) * w + x0, i01 = static_cast<size_t>(y0) * w + x1;
                const size_t i10 = static_cast<size_t>(y1) * w + x0, i11 = static_cast<size_t>(y1) * w + x1;
                const float ax = (curX[i00] + curX[i01] + curX[i10] + curX[i11]) * 0.25f;
                const float ay = (curY[i00] + curY[i01] + curY[i10] + curY[i11]) * 0.25f;
                const float az = (curZ[i00] + curZ[i01] + curZ[i10] + curZ[i11]) * 0.25f;
                const size_t o = static_cast<size_t>(y) * nw + x;
                nx[o] = ax; ny[o] = ay; nz[o] = az;
                r[o] = std::clamp(std::sqrt(ax * ax + ay * ay + az * az), 0.0f, 1.0f);
            }
        }
        result.push_back(std::move(r));
        curX.swap(nx); curY.swap(ny); curZ.swap(nz);
        w = nw; h = nh;
    }
    return result;
}

void bcCompressorInit() {
#ifdef ERUPTION_HAVE_BC_ENCODER
    std::call_once(g_initOnce, []() {
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
        bc7enc_compress_block_init();
    });
#endif
}

void setBcFormatSupport(const BcFormatSupport& s) { g_support = s; }
const BcFormatSupport& bcFormatSupport() { return g_support; }

EtexData compressRgbaToEtex(const uint8_t* rgba, int width, int height, BcKind kind,
                            bool hasAlpha, const std::vector<std::vector<float>>* roughnessBroadenR) {
    EtexData out;
#ifndef ERUPTION_HAVE_BC_ENCODER
    // Build sem encoder vendorizado: nada a comprimir, caminho RGBA8 assume.
    (void)rgba; (void)width; (void)height; (void)kind; (void)hasAlpha; (void)roughnessBroadenR;
    return out;
#else
    if (!rgba || width < 4 || height < 4) return out;
    bcCompressorInit();

    VkFormat fmt = VK_FORMAT_UNDEFINED;
    switch (kind) {
        case BcKind::Albedo:
            if (hasAlpha) {
                if (g_support.bc3) fmt = VK_FORMAT_BC3_UNORM_BLOCK;
            } else {
                if (g_support.bc1) fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
                else if (g_support.bc3) fmt = VK_FORMAT_BC3_UNORM_BLOCK;
            }
            break;
        case BcKind::Normal:
            // Nunca BC1 aqui (paleta RGB conjunta num normal = iluminação em
            // blocos). E NUNCA BC5 NESTA ENGINE: BC5 guarda só R,G — a amostra
            // volta com B=0 e A=1, e o shader faz `sampledNormal.xyz*2-1`, ou
            // seja z = -1. Os normal maps daqui NÃO são unitários de propósito
            // (azul ~239 para plano; ver a nota em gbuffer/model.frag) e o
            // alfa é materialProps, então reconstruir z de x,y mudaria o look.
            // BC7 guarda os quatro canais pelo mesmo 1 byte/texel do BC5.
            // (Medido: com BC5 aqui a cena dava RMS 9,7 e -8,5 de brilho.)
            if (g_support.bc7) fmt = VK_FORMAT_BC7_UNORM_BLOCK;
            break;
        case BcKind::Data:
            if (g_support.bc7) fmt = VK_FORMAT_BC7_UNORM_BLOCK;
            else if (g_support.bc3) fmt = VK_FORMAT_BC3_UNORM_BLOCK;
            break;
    }
    if (fmt == VK_FORMAT_UNDEFINED) return out; // caller falls back to RGBA8

    std::vector<uint8_t> level(static_cast<size_t>(width) * height * 4);
    std::memcpy(level.data(), rgba, level.size());

    // BC7 escolhe o MODO por bloco, e "bloco com alfa < 255" força o modo 6:
    // um único subconjunto, sem partições, e um índice de 4 bits COMPARTILHADO
    // entre RGB e A. Num normal map isso é o pior caso possível - x e y variam
    // em eixos independentes e passam a ter que caber numa reta só.
    //
    // O normal sintetizado grava alfa = 1 ("materialProps: nenhum"), constante,
    // e nenhum shader consome esse canal (é atribuído a materialProps e nunca
    // usado, em model.frag e terrain.frag). Alfa CONSTANTE não carrega
    // informação espacial: zerar para 255 libera os modos com partição e é a
    // diferença entre RMS 9,7 e RMS ~1 na cena. Se o alfa VARIA, é dado de
    // verdade e fica intocado.
    if (kind == BcKind::Normal) {
        bool uniformAlpha = true;
        const uint8_t a0 = level[3];
        for (size_t p = 3; p < level.size(); p += 4) {
            if (level[p] != a0) { uniformAlpha = false; break; }
        }
        if (uniformAlpha && a0 != 255) {
            for (size_t p = 3; p < level.size(); p += 4) level[p] = 255;
        }
    }

    int w = width, h = height;
    out.vkFormat = static_cast<uint32_t>(fmt);
    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);

    int mipLevel = 0;
    while (true) {
        // NITIDEZ ESPECULAR ADAPTATIVA (Toksvig), no bake, uma vez por mip -
        // nao por pixel por frame. `roughness` era CONSTANTE (204/255 ~ 0.8)
        // pra' toda textura sintetizada, entao mrahw nunca carregava variacao
        // real nenhuma; so' a tabela de nomes de material fazia diferenca.
        // Aqui o canal G de CADA texel deste mip e' aumentado proporcional
        // a' variancia da normal map QUE FOI PERDIDA reduzindo ate' este mip
        // (computeNormalMipVariance) - textura com relevo fino (pedra, telha)
        // fica mais fosca de longe (onde o mip e' usado), textura lisa
        // continua com a roughness autoral. E' o MESMO problema que o
        // specular AA em tela (pbr_common.glsl) resolve por pixel; aqui e'
        // resolvido uma vez, no mip certo, sem custo de frame nenhum.
        if (kind == BcKind::Data && roughnessBroadenR &&
            static_cast<size_t>(mipLevel) < roughnessBroadenR->size()) {
            const auto& rMap = (*roughnessBroadenR)[mipLevel];
            if (rMap.size() == static_cast<size_t>(w) * h) {
                for (size_t p = 0; p < rMap.size(); ++p) {
                    uint8_t& g = level[p * 4 + 1];
                    g = static_cast<uint8_t>(std::clamp(
                        toksvigRoughness(g / 255.0f, rMap[p]) * 255.0f, 0.0f, 255.0f));
                }
            }
        }
        std::vector<uint8_t> enc;
        encodeLevel(fmt, level, w, h, enc);
        out.mipSizes.push_back(static_cast<uint32_t>(enc.size()));
        out.payload.insert(out.payload.end(), enc.begin(), enc.end());
        if (w == 1 && h == 1) break;
        std::vector<uint8_t> next;
        int nw = 0, nh = 0;
        halveRgba(level, w, h, next, nw, nh);
        level.swap(next);
        w = nw; h = nh;
        ++mipLevel;
    }
    out.mipCount = static_cast<uint32_t>(out.mipSizes.size());
    return out;
#endif // ERUPTION_HAVE_BC_ENCODER
}

void appendEtexBytes(const EtexData& e, std::vector<uint8_t>& out) {
    auto put32 = [&out](uint32_t v) {
        const size_t o = out.size();
        out.resize(o + 4);
        std::memcpy(out.data() + o, &v, 4);
    };
    put32(kEtexMagic);
    put32(e.vkFormat);
    put32(e.width);
    put32(e.height);
    put32(e.mipCount);
    size_t off = 0;
    for (uint32_t i = 0; i < e.mipCount; ++i) {
        put32(e.mipSizes[i]);
        out.insert(out.end(), e.payload.begin() + off, e.payload.begin() + off + e.mipSizes[i]);
        off += e.mipSizes[i];
    }
}

bool readEtexBytes(const uint8_t* data, size_t size, size_t& cursor, EtexData& out) {
    auto get32 = [&](uint32_t& v) -> bool {
        if (cursor + 4 > size) return false;
        std::memcpy(&v, data + cursor, 4);
        cursor += 4;
        return true;
    };
    uint32_t magic = 0;
    if (!get32(magic) || magic != kEtexMagic) return false;
    EtexData e;
    if (!get32(e.vkFormat) || !get32(e.width) || !get32(e.height) || !get32(e.mipCount))
        return false;
    if (e.mipCount == 0 || e.mipCount > 16 || e.width == 0 || e.height == 0) return false;
    e.mipSizes.reserve(e.mipCount);
    for (uint32_t i = 0; i < e.mipCount; ++i) {
        uint32_t sz = 0;
        if (!get32(sz) || sz == 0 || cursor + sz > size) return false;
        const size_t base = e.payload.size();
        e.payload.resize(base + sz);
        std::memcpy(e.payload.data() + base, data + cursor, sz);
        cursor += sz;
        e.mipSizes.push_back(sz);
    }
    out = std::move(e);
    return true;
}

} // namespace eruption
