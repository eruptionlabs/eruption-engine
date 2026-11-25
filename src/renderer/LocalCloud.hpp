#pragma once

#include "math/Types.hpp"
#include <cstdint>
#include <cstring>

namespace eruption {

// A manually spawned elliptical cloud patch that is blended on top of the
// procedural cloud coverage. Local clouds drift with the wind, have their own
// density/altitude and affect both the volumetric cloud layer and precipitation
// occlusion.
// Nuvem de script: a borda NOMINAL e' kScriptCloudAreaScale x area_radius (2,6); o ruido da
// casca empurra a silhueta para dentro e a borda visivel (meia densidade) fica em ~1,55x, sempre alem da coluna
// de chuva (1,0x) e do disco de sombra (1,3x); o chao
// escurece num disco de kScriptCloudGroundScale x area_radius (borda suave).
constexpr float kScriptCloudAreaScale = 2.6f;
constexpr float kScriptCloudGroundScale = 1.3f;

struct LocalCloud {
    // Max baked blobs per coverage array == max spawnable independent clouds
    // (each cloud normally gets its own layer; merging only happens for same
    // altitude + same XZ). SSBO is unsized in coverage_gen.comp, so raising
    // this needs no shader change. Raised 20 -> 100 on request.
    static constexpr uint32_t MAX_COUNT = 100;
    // Politica do teto (documentada em Engine::spawnScriptCloud): as nuvens de
    // script (weather.cloud_spawn) dividem os MAX_COUNT com as do clima. Cota
    // garantida ao script: SCRIPT_QUOTA nuvens vivas ao mesmo tempo. O clima
    // cede o lugar (a nuvem de campo mais longe sai) quando o teto esta' cheio;
    // acima da cota o script recebe nil e um aviso no log.
    static constexpr uint32_t SCRIPT_QUOTA = 24;

    Vec2 center = Vec2(0.0f);    // world XZ
    Vec2 radius = Vec2(50.0f);   // ellipse semi-axes in meters
    float rotation = 0.0f;       // radians
    float density = 0.8f;        // 0..1 (puff vs full)
    float falloff = 1.0f;        // edge softness (higher = softer)
    float altitude = 0.5f;       // relative altitude within the layer [0,1]
    float coverage = 0.5f;       // 0..1 threshold on internal noise (higher = fuller)
    uint32_t layerMask = 1;      // bitmask of affected layers (max 9 bits)
    // std430 layout note: the GPU struct (coverage_gen.comp / cloud_billboard.frag)
    // reads `uint alive; uint _pad;` and its array stride rounds to 48 bytes.
    // A C++ `bool alive` packs to 44 bytes, which shifted every blob past the
    // first and corrupted composite clouds in the bake. Keep this 48 bytes.
    uint32_t alive = 1;
    uint32_t _pad = 0;

    // Nuvem de script: espessura vertical em metros (0 = nuvem do clima, a forma
    // de elipsoide de sempre). Mora em _pad, em bits de float, para manter os 48
    // bytes do layout std430; o shader a le com uintBitsToFloat(_pad).
    void setThickness(float meters) { std::memcpy(&_pad, &meters, sizeof(float)); }
    float thickness() const { float m; std::memcpy(&m, &_pad, sizeof(float)); return m; }
};

} // namespace eruption
