// Sem a ponte do LOD externo (LodSystemBridge.cpp, fora do repositorio): nenhuma
// malha ganha niveis proprios e o motor fica com o LOD comum de MeshLod. O
// CMake tira este arquivo do build quando LodSystemBridge.cpp existe.
#include "renderer/LodSystemBridge.hpp"

namespace eruption {

void lodBridgeSetPixelSource(LodPixelSource) {}
void lodBridgeSetSurfaceSource(LodPixelSource) {}
void lodBridgeSetSlotIdentity(LodSlotIdentity) {}

// Hash de 64 bits por palavra (8 bytes por passo), o da chave rapida de cache
// das texturas; a cauda de menos de 8 bytes entra byte a byte.
uint64_t lodBridgeFastHash(const void* data, size_t bytes, uint64_t seed) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint64_t h = seed ^ (uint64_t(bytes) * 0xFF51AFD7ED558CCDULL);
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        uint64_t w = 0;
        for (int k = 0; k < 8; ++k) w |= uint64_t(p[i + k]) << (8 * k);
        h = (h ^ w) * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 29;
    }
    for (; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001B3ULL;
    h ^= h >> 32;
    return h;
}

bool lodBridgeBuild(const std::vector<TerrainVertex>&, const std::vector<uint32_t>&,
                    std::vector<LodBridgeLevel>&) {
    return false;
}

} // namespace eruption
