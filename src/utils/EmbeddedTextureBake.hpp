#pragma once
#include <mutex>

#include "utils/BcCompressor.hpp"

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
EmbeddedBakeResult bakeEmbeddedTextures(const ModelFile& model,
                                        uint32_t maxTextureSize,
                                        bool synthesizePbr);

// So' o cache (<glb>.etexpack): sem bake, sem GPU, seguro fora da thread
// principal - a leitura de mapa em segundo plano (preload/warp) adianta o
// arquivo, e uma falta de cache volta ao bakeEmbeddedTextures na troca.
// Leitura de arquivo grande do disco, UM fio por vez (o resto do trabalho -
// parse, decode - segue em paralelo). Em HDD (e NTFS via FUSE) 16 leituras ao
// mesmo tempo faziam a cabeca pular entre arquivos: 60 modelos de cena, 125
// MB, em 7,2 s em paralelo contra ~2 s em sequencia (64 MB/s). Foi o grosso
// dos 97 s de abrir um mapa grande com 94 objetos de cena e o disco disputado.
std::mutex& diskReadMutex();

bool readEmbeddedBakeCache(const ModelFile& model, uint32_t maxTextureSize, bool synthesizePbr,
                           EmbeddedBakeResult& out);

// Build do pacote do mapa (com o vfs anotando a carga): o pacote de texturas
// que cada .glb usou e os nomes das imagens que ele traz, pela chave do vfs
// do .glb. O build tira essas imagens do .glb que vai no .emap.
struct EmbeddedPackCoverage {
    std::string pack; // chave do vfs
    std::vector<std::string> images;
};
std::unordered_map<std::string, EmbeddedPackCoverage> recordedPackCoverage();

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
