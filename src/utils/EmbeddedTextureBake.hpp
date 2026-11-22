#pragma once
#include <mutex>

#include "utils/BcCompressor.hpp"
#include "utils/PbrDeferredSource.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace eruption {

struct ModelFile;

// One embedded GLB image, block-compressed and ready to upload: albedo plus the
// PBR maps derived from it. Any of the three may be invalid, in which case the
// caller falls back to the RGBA8 path for that map only.
struct BakedEmbeddedTexture {
    EtexData albedo;
    EtexData normal;
    EtexData mrahw;
    bool albedoOpaque = false; // albedo sem alfa (o formato so' diz isso no BC1)

    // Leitura adiada (PbrDeferred.hpp): normal/mrahw ainda estao no arquivo do
    // pacote. `normal`/`mrahw` guardam so' o cabecalho (formato, tamanho, mips),
    // sem payload; quem precisa dos bytes chama normalData()/mrahwData(), que
    // le' o bloco na hora (so' acontece para o que o consumidor pede).
    std::shared_ptr<PbrDeferredSource> normalSrc;
    std::shared_ptr<PbrDeferredSource> mrahwSrc;
    bool pbrDeferred() const { return normalSrc || mrahwSrc; }
    const EtexData& normalData() const { return normalSrc ? normalSrc->load() : normal; }
    const EtexData& mrahwData() const { return mrahwSrc ? mrahwSrc->load() : mrahw; }
    // O mapa tem essa imagem (mesmo que ainda no arquivo).
    bool hasNormal() const { return normalSrc ? true : normal.valid(); }
    bool hasMrahw() const { return mrahwSrc ? true : mrahw.valid(); }
    // Bytes que o mapa tem dessa imagem (os do arquivo, quando adiada).
    size_t normalBytes() const { return normalSrc ? static_cast<size_t>(normalSrc->bytes) : normal.byteSize(); }
    size_t mrahwBytes() const { return mrahwSrc ? static_cast<size_t>(mrahwSrc->bytes) : mrahw.byteSize(); }
};

struct EmbeddedBakeResult {
    std::unordered_map<std::string, BakedEmbeddedTexture> textures;
    // Average albedo of the map (bounce/ambient ground tint). Computed while the
    // pixels are decoded and stored in the cache, so a cached load produces the
    // exact same lighting as a cold one instead of falling back to a constant.
    float groundAlbedo[3] = {0.0f, 0.0f, 0.0f};
    bool hasGroundAlbedo = false;
    bool fromCache = false;
    size_t bytes = 0; // total compressed payload
};

// Block-compress every texture embedded in `model` (GLB images), synthesizing
// the PBR pair from each albedo, and cache the whole thing next to the GLB as
// "<glb>.etexpack".
//
// The cache is a DERIVED artifact: it is keyed by the GLB's size+mtime, the
// resolution cap and the format set, so editing the GLB, changing the preset or
// running on a GPU without BC support all invalidate it automatically. Deleting
// it only costs one slower load. When no BC format is supported at all the
// function returns an empty result and the engine keeps its RGBA8 path.
//
// allowDeferredPbr: a carga a frio de um pacote grande em arquivo solto le' so' o
// albedo e deixa o PBR para depois (ver PbrDeferred.hpp). Quem passa true tem de
// tratar BakedEmbeddedTexture::pbrDeferred(); o padrao e' o carregamento completo.
EmbeddedBakeResult bakeEmbeddedTextures(const ModelFile& model,
                                        uint32_t maxTextureSize,
                                        bool synthesizePbr,
                                        bool allowDeferredPbr = false);

// So' o cache (<glb>.etexpack): sem bake, sem GPU, seguro fora da thread
// principal - a leitura de mapa em segundo plano (preload/warp) adianta o
// arquivo, e uma falta de cache volta ao bakeEmbeddedTextures na troca.
// Leitura de arquivo grande do disco, UM fio por vez (o resto do trabalho -
// parse, decode - segue em paralelo). Em HDD (e NTFS via FUSE) 16 leituras ao
// mesmo tempo faziam a cabeca pular entre arquivos: 60 modelos de cena, 125
// MB, em 7,2 s em paralelo contra ~2 s em sequencia (64 MB/s). Foi o grosso
// dos 97 s de abrir um mapa grande com 94 objetos de cena e o disco disputado.
std::mutex& diskReadMutex();

// allowDeferredPbr: como em bakeEmbeddedTextures, so' o albedo agora e o PBR depois
// (quem passa true trata BakedEmbeddedTexture::pbrDeferred()). Na troca de mapa a
// leitura de fundo lia o pacote inteiro ANTES da troca: com disco lento o jogador
// esperava de 5 a 23 s no mapa antigo (ver PbrDeferred.hpp).
bool readEmbeddedBakeCache(const ModelFile& model, uint32_t maxTextureSize, bool synthesizePbr,
                           EmbeddedBakeResult& out, bool allowDeferredPbr = false);

// Build do pacote do mapa (com o vfs anotando a carga): o pacote de texturas
// que cada .glb usou e os nomes das imagens que ele traz, pela chave do vfs
// do .glb. O build tira essas imagens do .glb que vai no .emap.
struct EmbeddedPackCoverage {
    std::string pack; // chave do vfs
    std::vector<std::string> images;
};
std::unordered_map<std::string, EmbeddedPackCoverage> recordedPackCoverage();

// Empacotamento .tuf com texturas prontas para a GPU (tufo --gpu-ready, ver
// docs/tuf.md): o pacote do .glb (`model.filePath`, no disco) em memoria, SEM
// gravar <glb>.etexpack. Usa o pacote em disco se ele valer para este .glb,
// preset e GPU (mesmos bytes do que o motor ja' usa); senao assa agora.
// `packPath` e' o caminho que o motor procura (<glb>.<teto><s|n>[.<fmt>].etexpack),
// `imageNames` os nomes das imagens que o pacote cobre. false: nada a assar.
bool buildEmbeddedBakePack(const ModelFile& model, uint32_t maxTextureSize, bool synthesizePbr,
                           std::vector<uint8_t>& packBytes, std::string& packPath,
                           std::vector<std::string>* imageNames = nullptr, bool* fromDiskCache = nullptr);
// As texturas de um pacote em memoria, sem validar contra o .glb (o extrator do
// .tuf remonta as imagens a partir dele). false se o formato nao fecha.
bool parseEmbeddedBakePackBytes(const std::vector<uint8_t>& packBytes, EmbeddedBakeResult& out);

// Apaga os pacotes (<glb>*.etexpack) deste GLB. O pacote e' chaveado so' pelo
// GLB (tamanho e data), entao um _mrahw/_normal novo em disco nao o invalida:
// sem apagar, a proxima carga usaria o PBR antigo guardado nele (alivio e LOD).
// Devolve quantos arquivos apagou.
size_t removeEmbeddedBakeCaches(const std::string& glbPath);

// Teto de resolucao ativo (preset de data/graphics.json). O loader de fundo
// (warp) nao le' o preset, entao o Engine publica o valor aqui.
void setEmbeddedTextureMaxSize(uint32_t maxSize);
uint32_t embeddedTextureMaxSize();

} // namespace eruption
