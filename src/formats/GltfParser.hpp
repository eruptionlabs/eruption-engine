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
};

} // namespace eruption
