#include "utils/TextureEncoder.hpp"

#include "utils/TextureFormatPolicy.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>

#ifdef ERUPTION_HAVE_BC_ENCODER
// Headers de terceiro: struct anonima dispara -Wpedantic que nao e' nosso.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "bc7decomp.h"
#include "bc7enc.h"
#include "rgbcx.h"
#pragma GCC diagnostic pop
#endif
#ifdef ERUPTION_HAVE_ASTC_ENCODER
#include "astcenc.h"
#endif
#ifdef ERUPTION_HAVE_ETC_ENCODER
#include "Decode.hpp"
#include "ProcessRGB.hpp"
#endif

namespace eruption::texenc {

namespace {

// Nivel copiado para um retangulo multiplo do bloco, repetindo a borda (mip
// menor que o bloco, 2x2 ou 1x1, vira um bloco bem definido).
[[maybe_unused]] std::vector<uint8_t> padTo(const uint8_t* rgba, int w, int h, int pw, int ph) {
    std::vector<uint8_t> out(static_cast<size_t>(pw) * ph * 4);
    for (int y = 0; y < ph; ++y) {
        const int sy = std::min(y, h - 1);
        for (int x = 0; x < pw; ++x) {
            const int sx = std::min(x, w - 1);
            std::memcpy(&out[(static_cast<size_t>(y) * pw + x) * 4], &rgba[(static_cast<size_t>(sy) * w + sx) * 4], 4);
        }
    }
    return out;
}

#ifdef ERUPTION_HAVE_BC_ENCODER
std::once_flag g_bcInit;
void initBc() {
    std::call_once(g_bcInit, [] {
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
        bc7enc_compress_block_init();
    });
}

bool encodeBc(VkFormat f, const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out, const Params& p) {
    initBc();
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    const texfmt::BlockInfo bi = texfmt::blockInfo(f);
    const std::vector<uint8_t> img = padTo(rgba, w, h, bw * 4, bh * 4);
    const int pw = bw * 4;
    out.resize(static_cast<size_t>(bw) * bh * bi.bytes);
    bc7enc_compress_block_params bc7p{};
    if (f == VK_FORMAT_BC7_UNORM_BLOCK) {
        bc7enc_compress_block_params_init(&bc7p);
        // Dados (MRAH-W, normal): o peso perceptual de luminancia gastaria bits
        // nos canais errados.
        if (p.linear) bc7enc_compress_block_params_init_linear_weights(&bc7p);
        // Orcamento de carga: as particoes dominam o custo; 16 guarda quase toda a
        // qualidade (BC3 em MRAH-W media RMS 9-14) por uma fracao do tempo de 64.
        bc7p.m_max_partitions = p.bc7Partitions;
        bc7p.m_uber_level = 0;
    }
    uint8_t block[64];
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            for (int y = 0; y < 4; ++y)
                std::memcpy(&block[y * 16], &img[(static_cast<size_t>(by * 4 + y) * pw + bx * 4) * 4], 16);
            uint8_t* dst = &out[(static_cast<size_t>(by) * bw + bx) * bi.bytes];
            switch (f) {
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
                rgbcx::encode_bc1(5, dst, block, true, false);
                break;
            case VK_FORMAT_BC3_UNORM_BLOCK:
                rgbcx::encode_bc3(5, dst, block);
                break;
            case VK_FORMAT_BC4_UNORM_BLOCK:
                rgbcx::encode_bc4(dst, block, 4); // canal R
                break;
            case VK_FORMAT_BC5_UNORM_BLOCK:
                rgbcx::encode_bc5(dst, block, 0, 1, 4); // R e G
                break;
            case VK_FORMAT_BC7_UNORM_BLOCK:
                bc7enc_compress_block(dst, block, &bc7p);
                break;
            default:
                return false;
            }
        }
    }
    return true;
}

bool decodeBc(VkFormat f, const uint8_t* blocks, int w, int h, std::vector<uint8_t>& rgba) {
    initBc();
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    const uint32_t bb = texfmt::blockInfo(f).bytes;
    uint8_t px[64];
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* b = blocks + (static_cast<size_t>(by) * bw + bx) * bb;
            for (int i = 0; i < 16; ++i) {
                px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = 0;
                px[i * 4 + 3] = 255;
            }
            switch (f) {
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: rgbcx::unpack_bc1(b, px); break;
            case VK_FORMAT_BC3_UNORM_BLOCK: rgbcx::unpack_bc3(b, px); break;
            case VK_FORMAT_BC4_UNORM_BLOCK: rgbcx::unpack_bc4(b, px, 4); break;
            case VK_FORMAT_BC5_UNORM_BLOCK: rgbcx::unpack_bc5(b, px, 0, 1, 4); break;
            case VK_FORMAT_BC7_UNORM_BLOCK: bc7decomp::unpack_bc7(b, reinterpret_cast<bc7decomp::color_rgba*>(px)); break;
            default: return false;
            }
            for (int y = 0; y < 4 && by * 4 + y < h; ++y)
                for (int x = 0; x < 4 && bx * 4 + x < w; ++x)
                    std::memcpy(&rgba[(static_cast<size_t>(by * 4 + y) * w + bx * 4 + x) * 4], &px[(y * 4 + x) * 4], 4);
        }
    }
    return true;
}
#endif // ERUPTION_HAVE_BC_ENCODER

#ifdef ERUPTION_HAVE_ASTC_ENCODER
// Um contexto por thread, bloco e qualidade: criar custa as tabelas do bloco
// (dezenas de ms), e o bake chama um nivel de mip por vez em varias threads.
struct AstcCtx {
    astcenc_context* ctx = nullptr;
    unsigned bx = 0, by = 0;
    float quality = 0.0f;
    bool decode = false;
    ~AstcCtx() {
        if (ctx) astcenc_context_free(ctx);
    }
};

astcenc_context* astcContext(unsigned bx, unsigned by, float quality, bool decodeOnly) {
    thread_local std::vector<std::unique_ptr<AstcCtx>> cache;
    for (const auto& c : cache)
        if (c->bx == bx && c->by == by && c->decode == decodeOnly && (decodeOnly || c->quality == quality)) return c->ctx;
    astcenc_config cfg{};
    const unsigned flags = decodeOnly ? ASTCENC_FLG_DECOMPRESS_ONLY : 0u;
    if (astcenc_config_init(ASTCENC_PRF_LDR, bx, by, 1, quality, flags, &cfg) != ASTCENC_SUCCESS) return nullptr;
    auto c = std::make_unique<AstcCtx>();
    if (astcenc_context_alloc(&cfg, 1, &c->ctx) != ASTCENC_SUCCESS) return nullptr;
    c->bx = bx;
    c->by = by;
    c->quality = quality;
    c->decode = decodeOnly;
    cache.push_back(std::move(c));
    return cache.back()->ctx;
}

bool encodeAstc(VkFormat f, const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out, const Params& p) {
    const texfmt::BlockInfo bi = texfmt::blockInfo(f);
    astcenc_context* ctx = astcContext(bi.w, bi.h, p.astcQuality, false);
    if (!ctx) return false;
    astcenc_image img{};
    img.dim_x = static_cast<unsigned>(w);
    img.dim_y = static_cast<unsigned>(h);
    img.dim_z = 1;
    img.data_type = ASTCENC_TYPE_U8;
    void* slice = const_cast<uint8_t*>(rgba); // o astcenc so' le' a entrada
    img.data = &slice;
    const astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    out.resize(levelBytes(f, w, h));
    const astcenc_error e = astcenc_compress_image(ctx, &img, &swz, out.data(), out.size(), 0);
    astcenc_compress_reset(ctx);
    return e == ASTCENC_SUCCESS;
}

bool decodeAstc(VkFormat f, const uint8_t* blocks, size_t size, int w, int h, std::vector<uint8_t>& rgba) {
    const texfmt::BlockInfo bi = texfmt::blockInfo(f);
    astcenc_context* ctx = astcContext(bi.w, bi.h, 0.0f, true);
    if (!ctx) return false;
    astcenc_image img{};
    img.dim_x = static_cast<unsigned>(w);
    img.dim_y = static_cast<unsigned>(h);
    img.dim_z = 1;
    img.data_type = ASTCENC_TYPE_U8;
    void* slice = rgba.data();
    img.data = &slice;
    const astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const astcenc_error e = astcenc_decompress_image(ctx, blocks, size, &img, &swz, 0);
    astcenc_decompress_reset(ctx);
    return e == ASTCENC_SUCCESS;
}
#endif // ERUPTION_HAVE_ASTC_ENCODER

#ifdef ERUPTION_HAVE_ETC_ENCODER
// O etcpak le' BGRA (o PNG dele e' carregado com png_set_bgr para ETC): R e B
// trocados na entrada; o bloco gravado e' o do padrao (teste 4 confere).
std::vector<uint32_t> bgraPadded(const uint8_t* rgba, int w, int h, int pw, int ph) {
    const std::vector<uint8_t> img = padTo(rgba, w, h, pw, ph);
    std::vector<uint32_t> out(static_cast<size_t>(pw) * ph);
    for (size_t i = 0; i < out.size(); ++i) {
        const uint8_t* c = &img[i * 4];
        const uint8_t bgra[4] = {c[2], c[1], c[0], c[3]};
        std::memcpy(&out[i], bgra, 4);
    }
    return out;
}

bool encodeEtc(VkFormat f, const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out) {
    const int pw = (w + 3) / 4 * 4, ph = (h + 3) / 4 * 4;
    const std::vector<uint32_t> src = bgraPadded(rgba, w, h, pw, ph);
    const uint32_t blocks = static_cast<uint32_t>((pw / 4) * (ph / 4));
    out.assign(levelBytes(f, w, h), 0);
    uint64_t* dst = reinterpret_cast<uint64_t*>(out.data());
    switch (f) {
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: CompressEtc2Rgb(src.data(), dst, blocks, pw, false); break;
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: CompressEtc2Rgba(src.data(), dst, blocks, pw, false); break;
    case VK_FORMAT_EAC_R11_UNORM_BLOCK: CompressEacR(src.data(), dst, blocks, pw); break;
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK: CompressEacRg(src.data(), dst, blocks, pw); break;
    default: return false;
    }
    return true;
}

bool decodeEtc(VkFormat f, const uint8_t* blocks, size_t size, int w, int h, std::vector<uint8_t>& rgba) {
    const int pw = (w + 3) / 4 * 4, ph = (h + 3) / 4 * 4;
    if (size < levelBytes(f, w, h)) return false;
    std::vector<uint64_t> src(size / 8);
    std::memcpy(src.data(), blocks, src.size() * 8);
    std::vector<uint32_t> dst(static_cast<size_t>(pw) * ph, 0);
    switch (f) {
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: DecodeRGB(src.data(), dst.data(), pw, ph); break;
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: DecodeRGBA(src.data(), dst.data(), pw, ph); break;
    case VK_FORMAT_EAC_R11_UNORM_BLOCK: DecodeR(src.data(), dst.data(), pw, ph); break;
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK: DecodeRG(src.data(), dst.data(), pw, ph); break;
    default: return false;
    }
    const bool oneChannel = f == VK_FORMAT_EAC_R11_UNORM_BLOCK;
    const bool twoChannel = f == VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
    const bool noAlpha = f == VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK || oneChannel || twoChannel;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t c[4];
            std::memcpy(c, &dst[static_cast<size_t>(y) * pw + x], 4);
            uint8_t* o = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            // O decodificador do etcpak devolve RGBA (so' a entrada do codificador
            // e' BGRA). Conferido no bloco: vermelho liso grava R1=31 no 1o byte
            // (ETC2 diferencial) e a palavra-base 0xFF (EAC R11).
            o[0] = c[0];
            o[1] = oneChannel ? 0 : c[1];
            o[2] = (oneChannel || twoChannel) ? 0 : c[2];
            o[3] = noAlpha ? 255 : c[3];
        }
    return true;
}
#endif // ERUPTION_HAVE_ETC_ENCODER

enum class Family { None, Bc, Astc, Etc };

Family familyOf(VkFormat f) {
    switch (f) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
        return Family::Bc;
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
    case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        return Family::Astc;
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        return Family::Etc;
    default:
        return Family::None;
    }
}

} // namespace

bool available(VkFormat f) {
    if (f == VK_FORMAT_R8G8B8A8_UNORM) return true; // "codificar" = copiar (modo raw)
    switch (familyOf(f)) {
#ifdef ERUPTION_HAVE_BC_ENCODER
    case Family::Bc: return true;
#endif
#ifdef ERUPTION_HAVE_ASTC_ENCODER
    case Family::Astc: return true;
#endif
#ifdef ERUPTION_HAVE_ETC_ENCODER
    case Family::Etc: return true;
#endif
    default: return false;
    }
}

size_t levelBytes(VkFormat f, int w, int h) {
    if (f == VK_FORMAT_R8G8B8A8_UNORM && w > 0 && h > 0) return static_cast<size_t>(w) * h * 4;
    const texfmt::BlockInfo bi = texfmt::blockInfo(f);
    if (bi.bytes == 0 || w < 1 || h < 1) return 0;
    return static_cast<size_t>((w + static_cast<int>(bi.w) - 1) / static_cast<int>(bi.w)) *
           static_cast<size_t>((h + static_cast<int>(bi.h) - 1) / static_cast<int>(bi.h)) * bi.bytes;
}

bool encodeLevel(VkFormat f, const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out, const Params& p) {
    if (!rgba || w < 1 || h < 1 || !available(f)) return false;
    if (f == VK_FORMAT_R8G8B8A8_UNORM) {
        out.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
        return true;
    }
    switch (familyOf(f)) {
#ifdef ERUPTION_HAVE_BC_ENCODER
    case Family::Bc: return encodeBc(f, rgba, w, h, out, p);
#endif
#ifdef ERUPTION_HAVE_ASTC_ENCODER
    case Family::Astc: return encodeAstc(f, rgba, w, h, out, p);
#endif
#ifdef ERUPTION_HAVE_ETC_ENCODER
    case Family::Etc: return encodeEtc(f, rgba, w, h, out);
#endif
    default: (void)out; (void)p; return false;
    }
}

bool decodeLevel(VkFormat f, const uint8_t* blocks, size_t size, int w, int h, std::vector<uint8_t>& rgba) {
    if (!blocks || w < 1 || h < 1 || size < levelBytes(f, w, h) || !available(f)) return false;
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    switch (familyOf(f)) {
#ifdef ERUPTION_HAVE_BC_ENCODER
    case Family::Bc: return decodeBc(f, blocks, w, h, rgba);
#endif
#ifdef ERUPTION_HAVE_ASTC_ENCODER
    case Family::Astc: return decodeAstc(f, blocks, size, w, h, rgba);
#endif
#ifdef ERUPTION_HAVE_ETC_ENCODER
    case Family::Etc: return decodeEtc(f, blocks, size, w, h, rgba);
#endif
    default: return false;
    }
}

} // namespace eruption::texenc
