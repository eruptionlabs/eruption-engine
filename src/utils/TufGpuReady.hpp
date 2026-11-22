#pragma once
// Texturas prontas para a GPU dentro do .tuf (tufo --gpu-ready, formato versao
// 2: tools/tufo docs/format.md secao 9; visao geral em docs/tuf.md).
//
// Empacotar: o .glb com imagens embutidas entra SEM elas (so' um marcador por
// imagem) e o pacote que o motor ja' usa (<glb>.<teto><s|n>[.<fmt>].etexpack:
// BC, mips, PBR resolvido) vai como entrada companheira. Ao carregar, o motor
// le' o pacote direto do .tuf (EmbeddedTextureBake::readCache, via vfs): nenhuma
// compressao e nenhum cache em disco. Extrair/visualizar: as imagens voltam
// decodificadas do BC (com perda), o .glb sai de novo com PNG embutidos.

#include "utils/TextureFormatPolicy.hpp"

#include "tufo/Archive.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace eruption {

// Registra o codec no Tufo (tufo::setGpuReadyCodec). Barato; chamar uma vez no
// inicio do processo (CLI --tuf-*, editor, jogo).
void registerTufGpuReadyCodec();

// Uma combinacao de textura que algum preset pode escolher (definida em
// core/Engine.cpp, engineTextureVariants).
struct TextureVariant {
    std::string preset; // presets que caem nesta combinacao, separados por virgula
    uint32_t maxSize = 1024;
    bool synthPbr = true;
    texfmt::Options options;
};
std::vector<TextureVariant> engineTextureVariants();

// Prepara o assador para o empacotamento (sem GPU nem janela): formatos BC
// completos e a lista de combinacoes de todos os presets (engineTextureVariants). Devolve false
// se o compressor BC nao foi compilado.
bool setupOfflineTextureBake();

// Textura de um .glb de pacote .tuf com texturas prontas, decodificada para a
// previa do editor: `glbKey`/`bytes` sao o .glb guardado, `companionBytes` o
// pacote. Devolve o nome e o PNG de cada imagem (so' o albedo).
struct TufDecodedImage {
    std::string name;
    int width = 0, height = 0;
    std::vector<uint8_t> rgba; // RGBA8, mip 0
};
// A entrada companheira (pacote de texturas) do .glb `key` num .tuf; false se nao ha'.
bool tufGpuCompanionKey(const tufo::Archive& ar, const std::string& key, std::string& companionKey);
bool decodeTufGpuImages(const std::vector<uint8_t>& companionBytes, std::vector<TufDecodedImage>& out);

} // namespace eruption
