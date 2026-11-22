#pragma once

#include "formats/ModelFile.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eruption {

// Parses a glTF 2.0 binary file (.glb) into the engine-native ModelFile structure.
// This is intentionally minimal: vertices, indices, normals, texcoords, node hierarchy,
// material/texture names and bounding boxes. Animations are parsed as a bonus when present.
class GltfParser {
public:
    // baseDir: diretório do .glb, necessário para imagens referenciadas por
    // URI (fluxo padrão glTF com texturas em arquivos separados).
    static ModelFile parse(const uint8_t* data, size_t size,
                           const std::string& baseDir = std::string());
    static ModelFile parse(const std::vector<uint8_t>& data,
                           const std::string& baseDir = std::string()) {
        return parse(data.data(), data.size(), baseDir);
    }

    // Nome que parse da' a' imagem `index` do glTF (o nome das texturas no
    // pacote .etexpack): o stem da URI, "#" + nome, ou "#image<index>".
    static std::string embeddedImageName(size_t index, const std::string& uri, const std::string& name);

    // Build do pacote do mapa: tira do .glb (`glb`, em memoria) os bytes das
    // imagens embutidas cujo nome `covered` aceita - as que o pacote de
    // texturas ja' traz comprimidas. No lugar fica um marcador com onde ler o
    // original no .glb do disco (`fileName`, ao lado). O resto sai byte a
    // byte igual: geometria, materiais, as entradas das imagens (nome, tipo)
    // e a ordem dos bufferViews. Devolve quantos bytes saíram (0: nada mudou).
    static size_t stripEmbeddedImages(std::vector<uint8_t>& glb, const std::string& fileName,
                                      const std::function<bool(const std::string&)>& covered);

    // O inverso de stripEmbeddedImages, para o extrator do .tuf: cada imagem
    // que ainda e' so' o marcador (ERUPSTRP) volta a ter bytes, os que
    // `bytesFor(nome da imagem)` devolve (vazio = continua o marcador). As
    // imagens passam a declarar "image/png". Devolve quantas foram repostas.
    static size_t restoreEmbeddedImages(std::vector<uint8_t>& glb,
                                        const std::function<std::vector<uint8_t>(const std::string&)>& bytesFor);

    // Imagens em arquivo (uri) cujo nome `covered` aceita: o pacote de texturas
    // as traz em formato de bloco, entao o arquivo nao precisa ir no .tuf. Marca
    // cada uma no JSON do .glb (extras.eruption_bc_only) para o parse aceitar a
    // falta do arquivo, e devolve as uris em `uris`. Devolve quantas marcou.
    static size_t markBcOnlyUriImages(std::vector<uint8_t>& glb, const std::function<bool(const std::string&)>& covered,
                                      std::vector<std::string>* uris);
    // Reescreve as imagens marcadas por markBcOnlyUriImages como PNG (uri -> bytes), para extrair.
    // Devolve as uris marcadas com o nome que embeddedImageName da' a cada uma.
    static std::vector<std::pair<std::string, std::string>> bcOnlyUriImages(const std::vector<uint8_t>& glb);
};

} // namespace eruption
