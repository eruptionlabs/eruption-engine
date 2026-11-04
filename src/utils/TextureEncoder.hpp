#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Codificadores de bloco do motor num ponto so' (spec 51, TC-03): BC1/BC3/BC4/
// BC5/BC7 (bc7enc_rdo + rgbcx), ASTC 4x4/6x6 (astcenc) e ETC2 RGB/RGBA e EAC
// R11/RG11 (etcpak). Um nivel de mip por chamada, a partir de RGBA8 compacto.
// Sem o codificador no build (ou formato desconhecido) devolve false e quem
// chama fica no RGBA8. Thread-safe: cada thread usa o proprio contexto ASTC.
namespace eruption::texenc {

struct Params {
    // Dados (MRAH-W, normal): erro igual por canal, sem peso de luminancia.
    bool linear = true;
    // BC7: particoes testadas por bloco (16 = o orcamento de carga de hoje).
    uint32_t bc7Partitions = 16;
    // ASTC: 0..100 (astcenc: 10 = fast, 60 = medium). Carga x qualidade.
    float astcQuality = 10.0f;
};

// Ha codificador para este formato neste build.
bool available(VkFormat f);
// Um nivel RGBA8 (w x h, 4 bytes por texel) em blocos de `f`. `out` recebe
// ceil(w/bw) x ceil(h/bh) blocos, na ordem das linhas.
bool encodeLevel(VkFormat f, const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out, const Params& p = {});
// Volta para RGBA8 (so' CPU: testes, ferramentas, leitura de alfa para o relevo).
// Canais que o formato nao guarda saem como a GPU devolve (BC4/EAC R: g=b=0, a=255;
// BC5/EAC RG: b=0, a=255; BC1/ETC2 RGB: a=255).
bool decodeLevel(VkFormat f, const uint8_t* blocks, size_t size, int w, int h, std::vector<uint8_t>& rgba);
// Bytes de um nivel w x h em `f` (0 = formato desconhecido).
size_t levelBytes(VkFormat f, int w, int h);

} // namespace eruption::texenc
