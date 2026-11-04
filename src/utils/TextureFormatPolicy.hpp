#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

// Escolha do formato de bloco de cada textura (spec 51, TC-02). Logica pura:
// recebe o que a GPU amostra (Caps), a classe da textura e as opcoes do preset,
// e devolve o VkFormat - VK_FORMAT_UNDEFINED quando nada serve e a textura
// fica em RGBA8 (o recuo de sempre). Sem Vulkan alem do enum: testavel sem GPU
// (tests/test_texture_codec.cpp).
//
// Familias e por que (not_commit/compressao_banda/03 e 04):
//  - BC (PC): BC1 no albedo opaco (0,5 B/texel), BC3 com alfa, BC7 nos dados de
//    4 canais (MRAH-W) e na normal de 4 canais do motor, BC5 na normal de 2
//    canais (opcao), BC4 em mascara de 1 canal.
//  - ASTC (celular): 4x4 (1 B/texel) em normal, dados, alfa e sprite - alfa
//    recortado e normal pedem bloco pequeno; albedo opaco em 6x6 (0,44 B/texel)
//    so' com a opcao, porque a perda depende da textura.
//  - ETC2/EAC (reserva do celular sem ASTC): ETC2 RGB no albedo opaco, ETC2 RGBA
//    (EAC no alfa) no resto, EAC R11 na mascara, EAC RG11 na normal de 2 canais.
namespace eruption::texfmt {

enum class Class : uint8_t {
    AlbedoOpaque, // cor sem alfa
    AlbedoAlpha,  // cor com alfa (recorte ou mistura)
    Normal,       // normal do motor: 4 canais (z sem reconstruir, alfa = materialProps)
    Data4,        // dados independentes por canal (MRAH-W)
    Mask1,        // um canal (altura, mascara)
    Sprite,       // quadro de sprite: pixel art, alfa duro, amostragem nearest
};
constexpr int kClassCount = 6;
const char* className(Class c);

enum class Mode : uint8_t {
    Off,  // tudo RGBA8 (o A/B de referencia)
    Auto, // a melhor familia que a GPU amostra: BC > ASTC > ETC2
    Bc,
    Astc,
    Etc2,
};
const char* modeName(Mode m);
// "off"|"auto"|"bc"|"astc"|"etc2" (e "0"/"1"); outro texto devolve `fallback`.
Mode parseMode(const std::string& s, Mode fallback);

// Formatos que a GPU amostra com filtro linear e aceita como destino de copia.
struct Caps {
    bool bc1 = false, bc3 = false, bc4 = false, bc5 = false, bc7 = false;
    bool astc4x4 = false, astc6x6 = false;
    bool etc2Rgb = false, etc2Rgba = false, eacR11 = false, eacRg11 = false;
    bool anyBc() const { return bc1 || bc3 || bc4 || bc5 || bc7; }
    bool anyAstc() const { return astc4x4 || astc6x6; }
    bool anyEtc2() const { return etc2Rgb || etc2Rgba || eacR11 || eacRg11; }
    bool any() const { return anyBc() || anyAstc() || anyEtc2(); }
};
// Lista separada por virgula (ex.: "bc1,bc3,astc4x4,etc2"): o ERUPTION_TEST_TEXTURE_CAPS
// que simula a GPU de um celular no PC. "etc2" liga ETC2 RGB e RGBA; "eac" liga R11 e RG11;
// "bc" liga os cinco BC; "astc" os dois ASTC; "none" nenhum.
Caps parseCaps(const std::string& list);
// Interseccao (simular nunca liga o que a GPU nao tem, se `real` for usado).
Caps intersect(const Caps& a, const Caps& b);
std::string describeCaps(const Caps& c);

struct Options {
    Mode mode = Mode::Auto;
    bool albedoBc7 = false;     // BC7 no albedo opaco (1 B/texel) em vez de BC1 (0,5)
    bool alphaBc7 = false;      // BC7 no albedo com alfa em vez de BC3 (mesmo tamanho)
    bool normalTwoChannel = false; // normal em 2 canais (BC5 / EAC RG11), z reconstruido no shader
    bool astcAlbedo6x6 = false; // ASTC 6x6 no albedo opaco (senao 4x4)
};

// Familia que o modo usa nesta GPU (Auto: BC > ASTC > ETC2; forcado: so' ela).
// Off ou nenhuma = Mode::Off.
Mode family(const Caps& caps, const Options& o);
// Formato de bloco da classe, ou VK_FORMAT_UNDEFINED (RGBA8).
VkFormat choose(Class c, const Caps& caps, const Options& o);

// Bytes por bloco e lado do bloco (4x4, ou o do ASTC); 0 = formato sem codificador.
struct BlockInfo {
    uint32_t bytes = 0;
    uint32_t w = 1, h = 1;
};
BlockInfo blockInfo(VkFormat f);
// O codificador do motor sabe produzir este formato (BC1/3/4/5/7, ASTC 4x4/6x6, ETC2/EAC).
bool encodable(VkFormat f);
// Rotulo curto ("BC7", "ASTC6x6", "ETC2A8", "RGBA8").
const char* formatTag(VkFormat f);
// Chave de 32 bits do conjunto (classe -> formato): entra no nome do cache, para
// que trocar de formato nao regrave o pacote do outro.
uint32_t policyKey(const Caps& caps, const Options& o);

// Estado do processo: capacidade sondada da GPU (init do motor) e opcoes do
// preset/ambiente (applyGraphicsPreset). Escrito na thread principal antes da
// carga; lido pelas threads do bake (copia sob trava).
void setDeviceCaps(const Caps& c);
void setOptions(const Options& o);
Caps deviceCaps();
Options options();
// Formato para a classe com o estado atual (UNDEFINED = fica RGBA8).
VkFormat chooseActive(Class c);
// Alguma classe comprime (falso: modo off, GPU sem nada, ou familia forcada ausente).
bool anyActive();
// O estado atual da' exatamente o pacote BC de antes desta spec (familia BC,
// albedo BC1/BC3, normal e dados BC7): o cache antigo continua valendo.
bool classicActive();
// Identificador curto do pacote fora do classico (8 hex da policyKey); vazio no classico.
std::string activeTag();
// A GPU amostra este formato (RGBA8 sempre; bloco so' se sondado).
bool deviceSamples(VkFormat f);

} // namespace eruption::texfmt
