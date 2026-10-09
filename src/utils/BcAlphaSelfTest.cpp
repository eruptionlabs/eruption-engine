#include "utils/BcAlphaSelfTest.hpp"
#include "utils/BcCompressor.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#ifdef ERUPTION_HAVE_BC_ENCODER
// Header vendorizado (bc7enc_rdo): usa struct aninhada anonima, que -Wpedantic
// reclama. bc7enc.cpp/rgbcx.cpp ja' ganham "-w" via set_source_files_properties
// no CMakeLists (SOURCE do arquivo), mas isto aqui e' um INCLUDE dentro de um
// arquivo NOSSO - a flag do .cpp deles nao alcanca. Silencia so' o include.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "bc7decomp.h"
#pragma GCC diagnostic pop
#endif

namespace eruption {
namespace {

struct AlphaStats {
    int lo = 255, hi = 0;
    double mean = 0.0, stdev = 0.0;
};

AlphaStats alphaStats(const std::vector<uint8_t>& rgba) {
    AlphaStats s;
    double sum = 0.0, sumSq = 0.0;
    const size_t n = rgba.size() / 4;
    for (size_t i = 0; i < n; ++i) {
        const int a = rgba[i * 4 + 3];
        s.lo = std::min(s.lo, a);
        s.hi = std::max(s.hi, a);
        sum += a;
        sumSq += static_cast<double>(a) * a;
    }
    s.mean = n ? sum / static_cast<double>(n) : 0.0;
    s.stdev = n ? std::sqrt(std::max(0.0, sumSq / static_cast<double>(n) - s.mean * s.mean)) : 0.0;
    return s;
}

// Comprime com a MESMA chamada que EmbeddedTextureBake.cpp usa pro canal de
// altura (BcKind::Data, hasAlpha=false, sem broadening de roughness), decodifica
// o mip 0 de volta e compara o alfa antes/depois. `src` e' RGBA8 width*height*4.
void testPattern(const char* label, int width, int height, const std::vector<uint8_t>& src) {
#ifndef ERUPTION_HAVE_BC_ENCODER
    (void)label; (void)width; (void)height; (void)src;
    ERUPTION_LOG_WARN("[BC7ALPHA] sem encoder BC vendorizado neste build (ERUPTION_HAVE_BC_ENCODER off) - pulado.");
#else
    bcCompressorInit();
    const EtexData enc = compressRgbaToEtex(src.data(), width, height, BcKind::Data,
                                            /*hasAlpha=*/false, nullptr);
    if (!enc.valid()) {
        ERUPTION_LOG_WARN("[BC7ALPHA] '%s': compressor nao produziu payload (sem BC7/BC3 suportado nesta maquina).", label);
        return;
    }

    // So' o mip 0 importa - e' o mesmo nivel que model.frag le no modo de
    // debug de altura (textureLod(..., 0.0)).
    const int bw = width / 4, bh = height / 4;
    const size_t blockBytes = enc.mipSizes[0] / (static_cast<size_t>(bw) * bh);
    std::vector<uint8_t> decoded(static_cast<size_t>(width) * height * 4);
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* block = &enc.payload[(static_cast<size_t>(by) * bw + bx) * blockBytes];
            bc7decomp::color_rgba pixels[16];
            bc7decomp::unpack_bc7(block, pixels);
            for (int yy = 0; yy < 4; ++yy) {
                for (int xx = 0; xx < 4; ++xx) {
                    uint8_t* dp = &decoded[((static_cast<size_t>(by * 4 + yy)) * width +
                                            (bx * 4 + xx)) * 4];
                    const auto& px = pixels[yy * 4 + xx];
                    dp[0] = px.r; dp[1] = px.g; dp[2] = px.b; dp[3] = px.a;
                }
            }
        }
    }

    const AlphaStats before = alphaStats(src);
    const AlphaStats after = alphaStats(decoded);
    const double stdDrop = before.stdev > 1e-6 ? (1.0 - after.stdev / before.stdev) * 100.0 : 0.0;
    ERUPTION_LOG_WARN("[BC7ALPHA] '%s' (%dx%d) ANTES  min=%d max=%d mean=%.1f std=%.1f",
                      label, width, height, before.lo, before.hi, before.mean, before.stdev);
    ERUPTION_LOG_WARN("[BC7ALPHA] '%s' (%dx%d) DEPOIS min=%d max=%d mean=%.1f std=%.1f (queda de contraste: %.1f%%)",
                      label, width, height, after.lo, after.hi, after.mean, after.stdev, stdDrop);
#endif
}

std::vector<uint8_t> makeRamp(int w, int h) {
    std::vector<uint8_t> img(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &img[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = 128; p[1] = 100; p[2] = 60;
            p[3] = static_cast<uint8_t>((x * 255) / std::max(1, w - 1));
        }
    }
    return img;
}

std::vector<uint8_t> makeGrain(int w, int h) {
    std::vector<uint8_t> img(static_cast<size_t>(w) * h * 4);
    uint32_t seed = 1234567u;
    auto rnd = [&]() -> uint8_t {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<uint8_t>(seed >> 24);
    };
    for (size_t i = 0; i < img.size() / 4; ++i) {
        img[i * 4 + 0] = 128; img[i * 4 + 1] = 100; img[i * 4 + 2] = 60;
        img[i * 4 + 3] = rnd();
    }
    return img;
}

std::vector<uint8_t> makeChecker(int w, int h) {
    std::vector<uint8_t> img(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &img[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = 128; p[1] = 100; p[2] = 60;
            p[3] = ((x / 4 + y / 4) % 2 == 0) ? 0 : 255;
        }
    }
    return img;
}

} // namespace

bool runBcAlphaSelfTestIfRequested() {
    const char* e = std::getenv("ERUPTION_TEST_BC7_ALPHA");
    if (!e || e[0] == '0') return false;

    ERUPTION_LOG_WARN("[BC7ALPHA] ida-e-volta sintetica do compressor de altura "
                      "(BcKind::Data, mesma chamada de EmbeddedTextureBake) - sem mapa, sem Vulkan.");
#ifdef ERUPTION_HAVE_BC_ENCODER
    // Roda ANTES do Vulkan existir, entao bcFormatSupport() ainda nao foi
    // preenchido por vkGetPhysicalDeviceFormatProperties (fica tudo falso por
    // padrao). O teste e' so' CPU (codificar+decodificar bytes), nao depende
    // de suporte de GPU nenhum - forca tudo "suportado" so' pra' o
    // compressor nao recusar por falta de formato.
    setBcFormatSupport(BcFormatSupport{true, true, true, true});
#endif
    const int W = 64, H = 64;
    testPattern("rampa linear", W, H, makeRamp(W, H));
    testPattern("ruido de grao", W, H, makeGrain(W, H));
    testPattern("xadrez 0/255", W, H, makeChecker(W, H));
    ERUPTION_LOG_WARN("[BC7ALPHA] fim.");
    return true;
}

} // namespace eruption
