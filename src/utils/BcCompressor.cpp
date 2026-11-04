#include "utils/BcCompressor.hpp"

#include "core/JobSystem.hpp"
#include "utils/TextureEncoder.hpp"
#include "utils/TextureFormatPolicy.hpp"

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
#include "bc7decomp.h"
#pragma GCC diagnostic pop
#endif

namespace eruption {

namespace {

BcFormatSupport g_support;
[[maybe_unused]] std::once_flag g_initOnce;

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

void setBcFormatSupport(const BcFormatSupport& s) {
    g_support = s;
    // Quem so' conhece o BC (autoteste, ferramentas) continua valendo: a
    // politica de formato (spec 51) le' os mesmos bits.
    texfmt::Caps c = texfmt::deviceCaps();
    c.bc1 = s.bc1;
    c.bc3 = s.bc3;
    c.bc5 = s.bc5;
    c.bc7 = s.bc7;
    texfmt::setDeviceCaps(c);
}
const BcFormatSupport& bcFormatSupport() { return g_support; }

EtexData rgbaToEtexRaw(const uint8_t* rgba, int width, int height) {
    EtexData out;
    if (!rgba || width < 1 || height < 1) return out;
    out.vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);
    std::vector<uint8_t> level(rgba, rgba + static_cast<size_t>(width) * height * 4);
    int w = width, h = height;
    for (;;) {
        out.payload.insert(out.payload.end(), level.begin(), level.end());
        out.mipSizes.push_back(static_cast<uint32_t>(level.size()));
        ++out.mipCount;
        if (w == 1 && h == 1) break;
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                const int x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                const int y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                for (int c = 0; c < 4; ++c) {
                    const int s = level[(static_cast<size_t>(y0) * w + x0) * 4 + c] +
                                  level[(static_cast<size_t>(y0) * w + x1) * 4 + c] +
                                  level[(static_cast<size_t>(y1) * w + x0) * 4 + c] +
                                  level[(static_cast<size_t>(y1) * w + x1) * 4 + c];
                    next[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((s + 2) / 4);
                }
            }
        }
        level.swap(next);
        w = nw;
        h = nh;
    }
    return out;
}

EtexData compressRgbaToEtex(const uint8_t* rgba, int width, int height, BcKind kind,
                            bool hasAlpha, const std::vector<std::vector<float>>* roughnessBroadenR,
                            float alphaTestCutoff, int coverageRegion) {
    EtexData out;
    if (!rgba || width < 4 || height < 4) return out;
    bcCompressorInit();

    // Formato pela politica (spec 51): classe da textura x formatos que a GPU
    // amostra x modo do preset. PC: albedo BC1 (BC3 com alfa), normal e dados
    // BC7 - o mesmo de antes. Celular: ASTC 4x4 (ou ETC2/EAC, reserva).
    //
    // Normal NUNCA em BC1/BC3 (paleta RGB conjunta = iluminacao em blocos) e,
    // por padrao, nunca em 2 canais (BC5/EAC RG): a amostra voltaria com B=0 e
    // A=1, e o shader faz `sampledNormal.xyz*2-1`, ou seja z = -1. Os normal
    // maps daqui NAO sao unitarios de proposito (azul ~239 para plano; ver a
    // nota em gbuffer/model.frag) e o alfa e' materialProps, entao reconstruir
    // z de x,y mudaria o look. (Medido: com BC5 aqui a cena dava RMS 9,7.)
    // BC7/ASTC 4x4 guardam os quatro canais pelo mesmo 1 byte/texel.
    texfmt::Class cls = texfmt::Class::Data4;
    if (kind == BcKind::Albedo) cls = hasAlpha ? texfmt::Class::AlbedoAlpha : texfmt::Class::AlbedoOpaque;
    else if (kind == BcKind::Normal) cls = texfmt::Class::Normal;
    const VkFormat fmt = texfmt::chooseActive(cls);
    if (fmt == VK_FORMAT_UNDEFINED || !texenc::available(fmt)) return out; // caller falls back to RGBA8

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
    // Cobertura do alfa de recorte por regiao (ver o cabecalho): alfa em float,
    // media 2x2 sem a escala, e o alvo de cada regiao tirado do mip 0.
    const bool cobertura = hasAlpha && alphaTestCutoff > 0.0f && kind == BcKind::Albedo;
    std::vector<float> af;
    std::vector<float> alvo;
    const int R0 = coverageRegion > 0 ? coverageRegion : std::max(width, height);
    const int nrx = (width + R0 - 1) / R0, nry = (height + R0 - 1) / R0;
    if (cobertura) {
        af.resize(static_cast<size_t>(w) * h);
        for (size_t p = 0; p < af.size(); ++p) af[p] = level[p * 4 + 3] / 255.0f;
        alvo.assign(static_cast<size_t>(nrx) * nry, 0.0f);
        std::vector<int> n(alvo.size(), 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t r = static_cast<size_t>(y / R0) * nrx + x / R0;
                alvo[r] += af[static_cast<size_t>(y) * w + x] >= 0.5f ? 1.0f : 0.0f;
                ++n[r];
            }
        for (size_t r = 0; r < alvo.size(); ++r) alvo[r] = n[r] ? alvo[r] / float(n[r]) : 0.0f;
    }
    auto aplicaCobertura = [&](int mip) {
        const int Rm = R0 >> mip;
        // regiao menor que um texel: o mip inteiro com o alvo medio
        const bool junta = Rm < 1;
        const int rw = junta ? w : Rm, rh = junta ? h : Rm;
        float alvoMedio = 0.0f;
        if (junta) { for (float c : alvo) alvoMedio += c; alvoMedio /= float(std::max<size_t>(1, alvo.size())); }
        std::vector<float> v;
        for (int ry = 0; ry < h; ry += rh)
            for (int rx = 0; rx < w; rx += rw) {
                const float c = junta ? alvoMedio : alvo[static_cast<size_t>(ry / Rm) * nrx + rx / Rm];
                v.clear();
                for (int y = ry; y < std::min(h, ry + rh); ++y)
                    for (int x = rx; x < std::min(w, rx + rw); ++x) v.push_back(af[static_cast<size_t>(y) * w + x]);
                const int nIn = int(std::lround(double(c) * v.size()));
                float limiar = 2.0f;   // nada passa
                if (nIn >= int(v.size())) {
                    float mn = 1.0f;
                    for (float a : v) if (a > 0.0f) mn = std::min(mn, a);
                    limiar = 0.5f * mn;
                } else if (nIn > 0) {
                    const size_t k = v.size() - size_t(nIn);   // primeiro que passa, em ordem crescente
                    std::nth_element(v.begin(), v.begin() + k, v.end());
                    const float acima = v[k];
                    const float abaixo = *std::max_element(v.begin(), v.begin() + k);
                    limiar = 0.5f * (abaixo + acima);
                    if (limiar <= 0.0f) limiar = 0.5f * acima;
                }
                const float esc = limiar > 1.5f ? 0.0f : alphaTestCutoff / std::max(limiar, 1e-4f);
                for (int y = ry; y < std::min(h, ry + rh); ++y)
                    for (int x = rx; x < std::min(w, rx + rw); ++x) {
                        const size_t p = static_cast<size_t>(y) * w + x;
                        level[p * 4 + 3] = static_cast<uint8_t>(std::lround(std::clamp(af[p] * esc, 0.0f, 1.0f) * 255.0f));
                    }
            }
    };
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
        if (cobertura) aplicaCobertura(mipLevel);
        std::vector<uint8_t> enc;
        if (!texenc::encodeLevel(fmt, level.data(), w, h, enc)) return EtexData{};
        out.mipSizes.push_back(static_cast<uint32_t>(enc.size()));
        out.payload.insert(out.payload.end(), enc.begin(), enc.end());
        if (w == 1 && h == 1) break;
        std::vector<uint8_t> next;
        int nw = 0, nh = 0;
        halveRgba(level, w, h, next, nw, nh);
        if (cobertura) {
            std::vector<float> an(static_cast<size_t>(nw) * nh);
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x) {
                    const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
                    const int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
                    an[static_cast<size_t>(y) * nw + x] = 0.25f * (af[static_cast<size_t>(y0) * w + x0] + af[static_cast<size_t>(y0) * w + x1] +
                                                                    af[static_cast<size_t>(y1) * w + x0] + af[static_cast<size_t>(y1) * w + x1]);
                }
            af.swap(an);
        }
        level.swap(next);
        w = nw; h = nh;
        ++mipLevel;
    }
    out.mipCount = static_cast<uint32_t>(out.mipSizes.size());
    return out;
}

bool decodeEtexMip0(const EtexData& e, std::vector<uint8_t>& rgba) {
    if (!e.valid() || e.mipSizes.empty() || e.width == 0 || e.height == 0) return false;
    const uint32_t w = e.width, h = e.height;
    const size_t n0 = e.mipSizes[0];
    if (n0 > e.payload.size()) return false;
    rgba.assign(size_t(w) * h * 4, 0);
    if (e.vkFormat == VK_FORMAT_R8G8B8A8_UNORM || e.vkFormat == VK_FORMAT_R8G8B8A8_SRGB) {
        if (n0 < rgba.size()) return false;
        std::memcpy(rgba.data(), e.payload.data(), rgba.size());
        return true;
    }
#ifdef ERUPTION_HAVE_BC_ENCODER
    const bool bc1 = e.vkFormat == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || e.vkFormat == VK_FORMAT_BC1_RGB_UNORM_BLOCK ||
                     e.vkFormat == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || e.vkFormat == VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    const bool bc3 = e.vkFormat == VK_FORMAT_BC3_UNORM_BLOCK || e.vkFormat == VK_FORMAT_BC3_SRGB_BLOCK;
    const bool bc7 = e.vkFormat == VK_FORMAT_BC7_UNORM_BLOCK || e.vkFormat == VK_FORMAT_BC7_SRGB_BLOCK;
    if (!bc1 && !bc3 && !bc7)
        return texenc::decodeLevel(static_cast<VkFormat>(e.vkFormat), e.payload.data(), n0, int(w), int(h), rgba);
    const size_t bloco = bc1 ? 8 : 16;
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (n0 < size_t(bw) * bh * bloco) return false;
    bcCompressorInit();
    uint8_t px[16 * 4];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = e.payload.data() + (size_t(by) * bw + bx) * bloco;
            if (bc1) rgbcx::unpack_bc1(b, px);
            else if (bc3) rgbcx::unpack_bc3(b, px);
            else bc7decomp::unpack_bc7(b, reinterpret_cast<bc7decomp::color_rgba*>(px));
            for (uint32_t y = 0; y < 4 && by * 4 + y < h; ++y)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < w; ++x)
                    std::memcpy(&rgba[((size_t(by) * 4 + y) * w + bx * 4 + x) * 4], &px[(y * 4 + x) * 4], 4);
        }
    return true;
#else
    return texenc::decodeLevel(static_cast<VkFormat>(e.vkFormat), e.payload.data(), n0, int(w), int(h), rgba);
#endif
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
