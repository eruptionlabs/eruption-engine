#pragma once

#include "formats/ModelFile.hpp"
#include <vector>
#include <cstdint>

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
};

} // namespace eruption
