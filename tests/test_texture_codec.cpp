// Spec 51 (texturas comprimidas): politica de formato (TC-02) e codificadores
// (TC-03) sem GPU. Ida-e-volta na CPU com o decodificador de referencia de cada
// biblioteca (bc7decomp/rgbcx, astcenc, etcpak).
#include "utils/TextureEncoder.hpp"
#include "renderer/LayerBlockRules.hpp"
#include "utils/TextureFormatPolicy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace eruption;

static int g_falhas = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_falhas;                                                              \
        }                                                                            \
    } while (0)

namespace {

// Imagem de teste reprodutivel: gradiente suave + textura de alta frequencia +
// bordas duras (o pior caso para bloco), em todos os canais.
std::vector<uint8_t> synth(int w, int h, uint32_t seed) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    uint32_t s = seed;
    auto rnd = [&s]() {
        s = s * 1664525u + 1013904223u;
        return (s >> 24) & 0xFFu;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            const float gx = float(x) / float(w), gy = float(y) / float(h);
            const int noise = int(rnd() % 24) - 12;
            const bool edge = ((x / 16) + (y / 16)) % 2 == 0;
            p[0] = uint8_t(std::clamp(int(200 * gx + 30 + noise + (edge ? 20 : 0)), 0, 255));
            p[1] = uint8_t(std::clamp(int(180 * gy + 40 + noise), 0, 255));
            p[2] = uint8_t(std::clamp(int(120 * (1 - gx) + 60 + noise / 2), 0, 255));
            p[3] = 255;
        }
    return px;
}

// PSNR (dB) nos canais [c0, c0+n).
double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int c0, int n) {
    double se = 0.0;
    size_t cnt = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = c0; c < c0 + n; ++c) {
            const double d = double(a[i + size_t(c)]) - double(b[i + size_t(c)]);
            se += d * d;
            ++cnt;
        }
    const double mse = se / double(cnt);
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

struct Case {
    VkFormat f;
    int c0, n;      // canais que o formato guarda
    double minDb;   // piso na imagem sintetica (com ruido de +-12 niveis)
};

} // namespace

// --bench N: custo de CPU do cozimento, por formato, numa imagem NxN com a cadeia
// de mips inteira (uma thread; o bake divide as texturas entre as threads).
static int bench(int n) {
    std::vector<uint8_t> img = synth(n, n, 11);
    const VkFormat fs[] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK,
                           VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
                           VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,
                           VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_EAC_R11_UNORM_BLOCK,
                           VK_FORMAT_EAC_R11G11_UNORM_BLOCK};
    for (VkFormat f : fs) {
        if (!texenc::available(f)) continue;
        std::vector<uint8_t> level = img, out;
        int w = n, h = n;
        size_t bytes = 0;
        double ms = 0.0, px = 0.0;
        for (;;) {
            const auto t0 = std::chrono::steady_clock::now();
            texenc::encodeLevel(f, level.data(), w, h, out);
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            bytes += out.size();
            px += double(w) * h;
            if (w == 1 && h == 1) break;
            const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
            std::vector<uint8_t> next(size_t(nw) * nh * 4);
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x)
                    for (int c = 0; c < 4; ++c)
                        next[(size_t(y) * nw + x) * 4 + c] = level[(size_t(std::min(2 * y, h - 1)) * w + std::min(2 * x, w - 1)) * 4 + c];
            level.swap(next);
            w = nw;
            h = nh;
        }
        std::printf("bench %-9s %dx%d com mips: %8.1f ms  %6.2f Mtexel/s  %.2f MB (RGBA8 %.2f MB)\n", texfmt::formatTag(f), n,
                    n, ms, px / ms / 1000.0, bytes / 1048576.0, px * 4 / 1048576.0);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--bench") return bench(std::max(4, std::atoi(argv[2])));
    using namespace eruption::texfmt;
    std::printf("1. modo e capacidade\n");
    CHECK(parseMode("off", Mode::Auto) == Mode::Off);
    CHECK(parseMode("AUTO", Mode::Off) == Mode::Auto);
    CHECK(parseMode("astc", Mode::Off) == Mode::Astc);
    CHECK(parseMode("etc2", Mode::Off) == Mode::Etc2);
    CHECK(parseMode("bc", Mode::Off) == Mode::Bc);
    CHECK(parseMode("lixo", Mode::Bc) == Mode::Bc);
    const Caps pc = parseCaps("bc");          // RTX/Intel: BC inteiro
    const Caps mali = parseCaps("astc,etc2,eac");
    const Caps etcOnly = parseCaps("etc2,eac");
    const Caps xclipse = parseCaps("bc1,bc3"); // o pacote que o motor ja' assava para esse celular
    const Caps none = parseCaps("none");
    CHECK(pc.anyBc() && !pc.anyAstc() && !pc.anyEtc2());
    CHECK(mali.astc4x4 && mali.astc6x6 && mali.etc2Rgba && mali.eacRg11 && !mali.anyBc());
    CHECK(!none.any());
    CHECK(describeCaps(none) == "nenhum");
    CHECK(intersect(pc, mali).any() == false);

    std::printf("2. politica: classe x capacidade x modo\n");
    Options o;
    // PC (auto): o que o motor ja' fazia.
    CHECK(choose(Class::AlbedoOpaque, pc, o) == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    CHECK(choose(Class::AlbedoAlpha, pc, o) == VK_FORMAT_BC3_UNORM_BLOCK);
    CHECK(choose(Class::Normal, pc, o) == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK(choose(Class::Data4, pc, o) == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK(choose(Class::Mask1, pc, o) == VK_FORMAT_BC4_UNORM_BLOCK);
    CHECK(choose(Class::Sprite, pc, o) == VK_FORMAT_BC7_UNORM_BLOCK);
    // Opcoes do PC.
    Options o2 = o;
    o2.albedoBc7 = true;
    o2.alphaBc7 = true;
    o2.normalTwoChannel = true;
    CHECK(choose(Class::AlbedoOpaque, pc, o2) == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK(choose(Class::AlbedoAlpha, pc, o2) == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK(choose(Class::Normal, pc, o2) == VK_FORMAT_BC5_UNORM_BLOCK);
    // Celular com ASTC (auto): 4x4 em tudo; 6x6 no albedo opaco so' com a opcao.
    CHECK(choose(Class::AlbedoOpaque, mali, o) == VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
    CHECK(choose(Class::Normal, mali, o) == VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
    CHECK(choose(Class::Sprite, mali, o) == VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
    Options o6 = o;
    o6.astcAlbedo6x6 = true;
    CHECK(choose(Class::AlbedoOpaque, mali, o6) == VK_FORMAT_ASTC_6x6_UNORM_BLOCK);
    CHECK(choose(Class::AlbedoAlpha, mali, o6) == VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
    // Forcar ETC2 no mesmo aparelho (reserva).
    Options oe = o;
    oe.mode = Mode::Etc2;
    CHECK(choose(Class::AlbedoOpaque, mali, oe) == VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK);
    CHECK(choose(Class::AlbedoAlpha, mali, oe) == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
    CHECK(choose(Class::Data4, mali, oe) == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
    CHECK(choose(Class::Mask1, mali, oe) == VK_FORMAT_EAC_R11_UNORM_BLOCK);
    oe.normalTwoChannel = true;
    CHECK(choose(Class::Normal, mali, oe) == VK_FORMAT_EAC_R11G11_UNORM_BLOCK);
    // Celular so' com ETC2 (auto cai nele).
    CHECK(choose(Class::AlbedoOpaque, etcOnly, o) == VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK);
    // Pacote para GPU so' com BC1/BC3: normal e dados sem BC7 ficam em RGBA8; mascara sem BC4 tambem.
    CHECK(choose(Class::AlbedoOpaque, xclipse, o) == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    CHECK(choose(Class::Normal, xclipse, o) == VK_FORMAT_UNDEFINED);
    CHECK(choose(Class::Data4, xclipse, o) == VK_FORMAT_BC3_UNORM_BLOCK);
    CHECK(choose(Class::Mask1, xclipse, o) == VK_FORMAT_UNDEFINED);
    // Recuo: modo off, GPU sem nada, ou familia forcada que a GPU nao tem.
    Options off = o;
    off.mode = Mode::Off;
    for (int c = 0; c < kClassCount; ++c) {
        CHECK(choose(static_cast<Class>(c), pc, off) == VK_FORMAT_UNDEFINED);
        CHECK(choose(static_cast<Class>(c), none, o) == VK_FORMAT_UNDEFINED);
    }
    Options forcedAstc = o;
    forcedAstc.mode = Mode::Astc;
    CHECK(family(pc, forcedAstc) == Mode::Off);
    CHECK(choose(Class::AlbedoOpaque, pc, forcedAstc) == VK_FORMAT_UNDEFINED);
    CHECK(family(mali, o) == Mode::Astc);
    CHECK(family(pc, o) == Mode::Bc);
    // Toda escolha tem codificador conhecido.
    for (const Caps* k : {&pc, &mali, &etcOnly, &xclipse})
        for (const Options* op : {&o, &o2, &o6, &oe})
            for (int c = 0; c < kClassCount; ++c) {
                const VkFormat f = choose(static_cast<Class>(c), *k, *op);
                CHECK(f == VK_FORMAT_UNDEFINED || encodable(f));
            }
    // Chave do cache: muda com o formato, nao com opcao que nao muda formato.
    CHECK(policyKey(pc, o) != policyKey(pc, o2));
    CHECK(policyKey(pc, o) != policyKey(mali, o));
    Options o6pc = o;
    o6pc.astcAlbedo6x6 = true; // nao muda nada no PC
    CHECK(policyKey(pc, o) == policyKey(pc, o6pc));

    std::printf("2b. array de camadas em bloco (TC-05): formato por familia\n");
    CHECK(layerFormat(pc, o) == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK(layerFormat(mali, o) == VK_FORMAT_ASTC_4x4_UNORM_BLOCK); // auto: ASTC antes de ETC2
    CHECK(layerFormat(mali, o6) == VK_FORMAT_ASTC_4x4_UNORM_BLOCK); // 6x6 e' so' do albedo: o array leva normal
    CHECK(layerFormat(mali, oe) == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
    CHECK(layerFormat(etcOnly, o) == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
    CHECK(layerFormat(xclipse, o) == VK_FORMAT_UNDEFINED); // BC sem BC7: fica RGBA8
    CHECK(layerFormat(pc, off) == VK_FORMAT_UNDEFINED);
    CHECK(layerFormat(none, o) == VK_FORMAT_UNDEFINED);
    CHECK(layerFormat(pc, forcedAstc) == VK_FORMAT_UNDEFINED);
    Options rawMode = o;
    rawMode.mode = Mode::Raw;
    CHECK(layerFormat(pc, rawMode) == VK_FORMAT_UNDEFINED);
    for (const Caps* k : {&pc, &mali, &etcOnly})
        for (const Options* op : {&o, &oe}) {
            const VkFormat f = layerFormat(*k, *op);
            CHECK(f == VK_FORMAT_UNDEFINED || (encodable(f) && blockInfo(f).bytes == 16 && texenc::available(f)));
        }
    // Memoria de uma camada com todos os mips: 1/4 do RGBA8 a 256 e a 128.
    for (const int side : {256, 128}) {
        size_t rgba = 0, blocks = 0;
        for (int s = side; s >= 1; s /= 2) {
            rgba += size_t(s) * s * 4;
            blocks += texenc::levelBytes(VK_FORMAT_BC7_UNORM_BLOCK, s, s);
            CHECK(texenc::levelBytes(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, s, s) ==
                  texenc::levelBytes(VK_FORMAT_BC7_UNORM_BLOCK, s, s));
            CHECK(texenc::levelBytes(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, s, s) ==
                  texenc::levelBytes(VK_FORMAT_BC7_UNORM_BLOCK, s, s));
        }
        std::printf("   camada %d com mips: RGBA8 %zu B, bloco %zu B (%.3f)\n", side, rgba, blocks,
                    double(blocks) / double(rgba));
        CHECK(blocks * 4 <= rgba + 128); // so' os mips menores que o bloco (2x2, 1x1) passam de 1/4
    }

    std::printf("2c. camadas em bloco: o que vai para o A e quando a copia e' direta\n");
    {
        using namespace eruption::layerrules;
        CHECK(wantsBlock(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, false));
        CHECK(wantsBlock(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, false));
        CHECK(wantsBlock(VK_FORMAT_R8G8B8A8_SRGB, false));      // cor sRGB sem compressao
        CHECK(!wantsBlock(VK_FORMAT_R8G8B8A8_UNORM, false));    // dado linear fica RGBA8
        CHECK(!wantsBlock(VK_FORMAT_R8_UNORM, false));
        CHECK(!wantsBlock(VK_FORMAT_BC7_UNORM_BLOCK, true));    // sem filtro (sprite) fica RGBA8
        CHECK(blockClass(VK_FORMAT_BC7_SRGB_BLOCK) == VK_FORMAT_BC7_UNORM_BLOCK);
        CHECK(blockClass(VK_FORMAT_ASTC_4x4_SRGB_BLOCK) == VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
        Source src;
        src.imageFormat = src.viewFormat = VK_FORMAT_BC7_UNORM_BLOCK;
        src.width = src.height = 512;
        src.mips = 10;
        src.levelCount = VK_REMAINING_MIP_LEVELS;
        src.transferSrc = true;
        uint32_t mip = 99;
        CHECK(directCopy(src, VK_FORMAT_BC7_UNORM_BLOCK, 256, 9, mip) && mip == 1); // 512 -> pula 1 mip
        CHECK(directCopy(src, VK_FORMAT_BC7_UNORM_BLOCK, 128, 8, mip) && mip == 2);
        Source s2 = src;
        s2.width = s2.height = 256;
        s2.mips = 9;
        CHECK(directCopy(s2, VK_FORMAT_BC7_UNORM_BLOCK, 256, 9, mip) && mip == 0);
        CHECK(!directCopy(s2, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 256, 9, mip)); // outra classe
        s2.mips = 5;
        CHECK(!directCopy(s2, VK_FORMAT_BC7_UNORM_BLOCK, 256, 9, mip)); // mips curtos
        s2 = src;
        s2.height = 256;
        CHECK(!directCopy(s2, VK_FORMAT_BC7_UNORM_BLOCK, 256, 9, mip)); // nao quadrada
        s2 = src;
        s2.width = s2.height = 128;
        s2.mips = 8;
        CHECK(!directCopy(s2, VK_FORMAT_BC7_UNORM_BLOCK, 256, 9, mip)); // menor que a camada: codifica
        s2 = src;
        s2.transferSrc = false;
        CHECK(!directCopy(s2, VK_FORMAT_BC7_UNORM_BLOCK, 256, 9, mip));
        s2 = src;
        s2.width = s2.height = 384; // nao e' potencia de 2
        CHECK(!directCopy(s2, VK_FORMAT_BC7_UNORM_BLOCK, 256, 9, mip));
        s2 = src;
        s2.viewFormat = VK_FORMAT_BC7_SRGB_BLOCK; // view sRGB da mesma imagem: mesmos bits
        CHECK(directCopy(s2, VK_FORMAT_BC7_UNORM_BLOCK, 256, 9, mip));
        s2 = src;
        s2.baseMip = 1; // view que comeca no mip 1 (256): copia do mip 1
        s2.levelCount = 9;
        CHECK(directCopy(s2, VK_FORMAT_BC7_UNORM_BLOCK, 256, 9, mip) && mip == 1);
        s2.levelCount = 4;
        CHECK(!directCopy(s2, VK_FORMAT_BC7_UNORM_BLOCK, 256, 9, mip));
        CHECK(!directCopy(src, VK_FORMAT_UNDEFINED, 256, 9, mip));
        // ETC2 RGB (albedo opaco da familia) num A em ETC2 RGBA: so' pelo caminho do alfa.
        Source e = src;
        e.imageFormat = e.viewFormat = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        CHECK(!directCopy(e, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 256, 9, mip));
        CHECK(etc2AddAlpha(e, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 256, 9, mip) && mip == 1);
        CHECK(!etc2AddAlpha(e, VK_FORMAT_BC7_UNORM_BLOCK, 256, 9, mip));
        CHECK(!etc2AddAlpha(src, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 256, 9, mip));
    }

    std::printf("2d. ETC2 RGB -> RGBA com alfa 255: mesmo RGB, alfa 255 em todo texel\n");
    if (texenc::available(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK) && texenc::available(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK)) {
        const int w = 64, h = 64;
        std::vector<uint8_t> img(size_t(w) * h * 4);
        uint32_t st = 12345u;
        for (size_t i = 0; i < img.size(); ++i) {
            st = st * 1664525u + 1013904223u;
            img[i] = (i % 4 == 3) ? 255 : uint8_t((st >> 24) / 2 + ((i / 4) % w) * 2);
        }
        std::vector<uint8_t> rgb, rgba(size_t(w / 4) * (h / 4) * 16), a, b;
        CHECK(texenc::encodeLevel(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, img.data(), w, h, rgb));
        CHECK(rgb.size() * 2 == rgba.size());
        eruption::layerrules::etc2RgbToRgba(rgb.data(), rgb.size() / 8, rgba.data());
        CHECK(texenc::decodeLevel(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, rgb.data(), rgb.size(), w, h, a));
        CHECK(texenc::decodeLevel(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, rgba.data(), rgba.size(), w, h, b));
        size_t diff = 0, alphaOff = 0;
        for (size_t px = 0; px < size_t(w) * h; ++px) {
            for (int c = 0; c < 3; ++c) diff += a[px * 4 + size_t(c)] != b[px * 4 + size_t(c)];
            alphaOff += b[px * 4 + 3] != 255;
        }
        CHECK(diff == 0);
        CHECK(alphaOff == 0);
    }

    std::printf("3. codificadores: ida-e-volta na CPU\n");
    const Case cases[] = {
        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0, 3, 30.0}, {VK_FORMAT_BC3_UNORM_BLOCK, 0, 4, 30.0},
        {VK_FORMAT_BC4_UNORM_BLOCK, 0, 1, 38.0},      {VK_FORMAT_BC5_UNORM_BLOCK, 0, 2, 38.0},
        {VK_FORMAT_BC7_UNORM_BLOCK, 0, 4, 36.0},      {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 0, 4, 34.0},
        {VK_FORMAT_ASTC_6x6_UNORM_BLOCK, 0, 3, 28.0}, {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 0, 3, 30.0},
        {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 0, 4, 30.0}, {VK_FORMAT_EAC_R11_UNORM_BLOCK, 0, 1, 38.0},
        {VK_FORMAT_EAC_R11G11_UNORM_BLOCK, 0, 2, 38.0},
    };
    const int W = 128, H = 96;
    const std::vector<uint8_t> img = synth(W, H, 7);
    for (const Case& c : cases) {
        if (!texenc::available(c.f)) {
            std::printf("   %-9s sem codificador neste build (pulado)\n", formatTag(c.f));
            continue;
        }
        std::vector<uint8_t> blocks, back;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(texenc::encodeLevel(c.f, img.data(), W, H, blocks));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(blocks.size() == texenc::levelBytes(c.f, W, H));
        CHECK(texenc::decodeLevel(c.f, blocks.data(), blocks.size(), W, H, back));
        const double db = psnr(img, back, c.c0, c.n);
        std::printf("   %-9s %6zu bytes (%.2f B/texel) PSNR %.2f dB  %.2f ms\n", formatTag(c.f), blocks.size(),
                    double(blocks.size()) / (W * H), db, ms);
        CHECK(db >= c.minDb);
    }

    std::printf("4. canais na ordem certa (cores puras, bloco liso)\n");
    const uint8_t pure[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {40, 120, 200, 255}};
    for (const Case& c : cases) {
        if (!texenc::available(c.f)) continue;
        for (const auto& col : pure) {
            std::vector<uint8_t> flat(8 * 8 * 4), blocks, back;
            for (size_t i = 0; i < flat.size(); i += 4) std::memcpy(&flat[i], col, 4);
            CHECK(texenc::encodeLevel(c.f, flat.data(), 8, 8, blocks));
            CHECK(texenc::decodeLevel(c.f, blocks.data(), blocks.size(), 8, 8, back));
            for (int ch = c.c0; ch < c.c0 + c.n; ++ch) {
                const int d = std::abs(int(back[size_t(ch)]) - int(col[ch]));
                if (d > 10) std::printf("   %s cor (%d,%d,%d) canal %d: %d\n", formatTag(c.f), col[0], col[1], col[2], ch,
                                        back[size_t(ch)]);
                CHECK(d <= 10);
            }
        }
    }

    std::printf("5. tamanhos fora do bloco (13x7, 2x2, 1x1)\n");
    for (const Case& c : cases) {
        if (!texenc::available(c.f)) continue;
        for (const auto& wh : {std::pair<int, int>{13, 7}, {2, 2}, {1, 1}}) {
            const std::vector<uint8_t> small = synth(wh.first, wh.second, 3);
            std::vector<uint8_t> blocks, back;
            CHECK(texenc::encodeLevel(c.f, small.data(), wh.first, wh.second, blocks));
            CHECK(blocks.size() == texenc::levelBytes(c.f, wh.first, wh.second));
            CHECK(texenc::decodeLevel(c.f, blocks.data(), blocks.size(), wh.first, wh.second, back));
            CHECK(back.size() == small.size());
        }
    }
    CHECK(texenc::levelBytes(VK_FORMAT_ASTC_6x6_UNORM_BLOCK, 256, 256) == 43u * 43u * 16u);
    CHECK(texenc::levelBytes(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 1, 1) == 8u);
    std::vector<uint8_t> rawOut;
    CHECK(!texenc::encodeLevel(VK_FORMAT_R16G16B16A16_SFLOAT, img.data(), W, H, rawOut)); // sem codificador
    // Modo raw: RGBA8 "codificado" = copia exata (referencia que isola o codec).
    CHECK(texenc::encodeLevel(VK_FORMAT_R8G8B8A8_UNORM, img.data(), W, H, rawOut) && rawOut == img);
    Options raw = o;
    raw.mode = Mode::Raw;
    CHECK(parseMode("raw", Mode::Off) == Mode::Raw);
    for (int c = 0; c < kClassCount; ++c) CHECK(choose(static_cast<Class>(c), none, raw) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(policyKey(pc, raw) != policyKey(pc, o));

    std::printf(g_falhas ? "HOUVE FALHA (%d)\n" : "OK\n", g_falhas);
    return g_falhas ? 1 : 0;
}
