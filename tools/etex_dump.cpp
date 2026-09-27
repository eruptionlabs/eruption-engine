// Despeja um mip de um .etex (BC7) para PNG - ferramenta de diagnóstico.
// Build: g++ -O2 tools/etex_dump.cpp third_party/bc7enc_rdo/bc7decomp.cpp \
//        -I third_party/bc7enc_rdo -I third_party/stb -o /tmp/etex_dump
// Uso: etex_dump arquivo.etex mip saida.png
#include "bc7decomp.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "uso: %s in.etex mip out.png\n", argv[0]); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    uint32_t h5[5];
    fread(h5, 4, 5, f);
    if (h5[0] != 0x31585445) { fprintf(stderr, "magic ruim\n"); return 1; }
    const uint32_t fmt = h5[1], W = h5[2], H = h5[3], mips = h5[4];
    const int want = atoi(argv[2]);
    fprintf(stderr, "fmt=%u %ux%u mips=%u\n", fmt, W, H, mips);
    if (fmt != 145) { fprintf(stderr, "só BC7 (145)\n"); return 1; }
    std::vector<uint8_t> level;
    uint32_t lw = 0, lh = 0;
    for (uint32_t i = 0; i < mips; ++i) {
        uint32_t sz; fread(&sz, 4, 1, f);
        std::vector<uint8_t> buf(sz);
        fread(buf.data(), 1, sz, f);
        if ((int)i == want) {
            level = buf;
            lw = W >> i; if (lw < 1) lw = 1;
            lh = H >> i; if (lh < 1) lh = 1;
        }
    }
    fclose(f);
    if (level.empty()) { fprintf(stderr, "mip fora do range\n"); return 1; }
    const uint32_t pw = (lw + 3) & ~3u, ph = (lh + 3) & ~3u;
    std::vector<uint8_t> rgba(pw * ph * 4);
    const uint32_t bw = pw / 4, bh = ph / 4;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            bc7decomp::color_rgba px[16];
            bc7decomp::unpack_bc7(level.data() + (by * bw + bx) * 16, px);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    uint32_t dx = bx * 4 + x, dy = by * 4 + y;
                    memcpy(&rgba[(dy * pw + dx) * 4], &px[y * 4 + x], 4);
                }
        }
    // recorta o pad
    std::vector<uint8_t> out(lw * lh * 4);
    for (uint32_t y = 0; y < lh; ++y)
        memcpy(&out[y * lw * 4], &rgba[y * pw * 4], lw * 4);
    stbi_write_png(argv[3], lw, lh, 4, out.data(), lw * 4);
    fprintf(stderr, "mip %d: %ux%u -> %s\n", want, lw, lh, argv[3]);
    return 0;
}
