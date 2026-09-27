#pragma once

#include "formats/MapLoader.hpp"

#include <vk_mem_alloc.h>
#include <vector>
#include <string>
#include <cstdlib>
#include <cstring>

#include "core/Logger.hpp"
#include "renderer/DeferredLighting.hpp"

// Helpers de escopo de arquivo que o Engine.cpp compartilhava com o codigo que
// saiu dele na quebra em varios .cpp (2026-09-04). Antes eram `static` no
// unico .cpp gigante e qualquer funcao alcancava; com a classe espalhada em
// Engine.cpp / ImGui.cpp / Render.cpp / ... precisam de um lugar
// comum. So' entra aqui o que e' usado por MAIS DE UM desses arquivos - o que
// e' usado por um so' foi junto com ele.

namespace eruption {

// Nomes das fases de CPU marcadas dentro de Engine::render(). Eram locais do
// relatorio de benchmark; viraram constante porque a telemetria por frame passou
// a exportar a mesma quebra - sem ela, render_ms e' um numero cego, e foi
// exatamente isso que escondeu que o gargalo real e' CPU, nao GPU.
//
// Usado por logBenchmarkBreakdown() e recordTelemetry() (Engine.cpp) e pelo
// painel de telemetria F3 (ImGui.cpp). `inline` porque C++17 permite,
// e assim o array e' UM so' em todas as unidades de traducao.
inline const char* const kCpuPhaseNames[] = {
    "FrameStart", "MapUpload", "Shadow", "Minimap", "GBuffer", "CloudShadowMap",
    "Deferred", "Water", "SkyCloudSprite", "CloudShadows(field)",
    "CloudVolumes(field)", "RainTopDown", "CloudLayers", "Post",
    "CopyToSwap", "EndFrame"
};

// Nivel/altura de onda da agua, vindo dos planos de agua do terreno moderno
// ou do descritor do mapa. Devolve true quando veio do terreno.
// ATENCAO: type==0 no terreno E' agua valida (textura "water0"), nao "sem
// agua". Com force=true e mapa sem info de agua devolve nivel 0, pra chave de
// debug "Force Water" funcionar tambem em mapa sem plano de agua.
//
// Compartilhado entre Engine.cpp e Render.cpp desde a quebra do arquivo.
inline bool getMapWaterParams(const LoadedMap& map, float& outLevel, float& outWaveHeight, bool force = false) {
    if (!map.waterPlanes.empty()) {
        outLevel = map.waterPlanes[0].level;
        outWaveHeight = map.waterPlanes[0].waveHeight;
        return true;
    } else if (map.waterPlanes.empty() ? 0.0f : map.waterPlanes[0].level != 0.0f || map.waterPlanes.empty() ? 0.2f : map.waterPlanes[0].waveHeight != 0.2f) {
        // Legacy world water info present
        outLevel = map.waterPlanes.empty() ? 0.0f : map.waterPlanes[0].level;
        outWaveHeight = map.waterPlanes.empty() ? 0.2f : map.waterPlanes[0].waveHeight;
        return false;
    } else if (force) {
        // No water info but Force Water is on: use default level 0
        outLevel = 0.0f;
        outWaveHeight = 0.2f;
        return false;
    } else {
        outLevel = 0.0f;
        outWaveHeight = 0.2f;
        return false;
    }
}

// Filas de upload adiadas para o command buffer do frame. Ficam aqui porque
// sao escritas na resolucao de textura/malha (Engine.cpp) e consumidas no
// upload do mapa (MapUpload.cpp) e no shutdown - tres arquivos desde a
// quebra. `inline` (C++17) mantem UMA instancia so' entre as unidades.
struct PendingMesh { VkBuffer staging; VmaAllocation alloc; VkBuffer vb; VmaAllocation vbAlloc; VkBuffer ib; VmaAllocation ibAlloc; VkDeviceSize vSize; VkDeviceSize iSize; };
inline std::vector<PendingMesh> s_pendingMeshes;

struct PendingUpload { VkImage image; VmaAllocation imageAlloc; VkBuffer staging; VmaAllocation stagingAlloc; uint32_t width, height, mipLevels;
    // Nao-vazio = mips ja' comprimidos no staging (uma copia por mip, sem
    // blit chain). Vazio = RGBA8 legado (uma copia + blit).
    std::vector<uint32_t> mipSizes; };
inline std::vector<PendingUpload> s_pendingUploads;

// Quando o terreno amostrado esta' abaixo do nivel da agua, nasce o jogador/
// camera NA SUPERFICIE, senao o sprite some debaixo d'agua (ex. centro do
// instancia-A). Usado pelo swap de mapa e pelo loadMap.
inline float clampSpawnYToWater(const LoadedMap& map, float terrainY) {
    if (map.waterPlanes.empty()) return terrainY;
    float waterY = map.waterPlanes[0].level;
    return (terrainY < waterY) ? waterY : terrainY;
}

// Debug helper: append artificial point lights around the map center when
// ERUPTION_TEST_ARTIFICIAL_LIGHTS=1 is set. Only the default test map is wired up, so other maps
// keep their original terrain lighting untouched.
inline void appendArtificialTestLights(const std::string& mapName,
                                       float centerX, float centerZ,
                                       std::vector<PointLight>& lights) {
    const char* env = std::getenv("ERUPTION_TEST_ARTIFICIAL_LIGHTS");
    if (!env || std::strcmp(env, "1") != 0) return;
    if (mapName != "parana_field") return;

    const auto add = [&](float x, float y, float z, float r, float g, float b,
                         float intensity, float radius) {
        PointLight pl;
        pl.position = Vec3(centerX + x, y, centerZ + z);
        pl.color = Vec3(r, g, b);
        pl.intensity = intensity;
        pl.radius = radius;
        pl.animType = LightAnimType::Static;
        pl.enabled = true;
        lights.push_back(pl);
    };

    // ~8 soft point lights around the test map center/spawn (~750, 74, 900).
    // Heights stay above the terrain (y ~80-100) so they illuminate walls,
    // houses, the gate and trees instead of sitting underground.
    add(   0.0f,  90.0f,    0.0f, 1.00f, 0.80f, 0.55f, 1.20f, 100.0f); // warm white center
    add( -80.0f,  85.0f,  +60.0f, 1.00f, 0.55f, 0.10f, 1.50f,  80.0f); // orange near houses
    add( +70.0f,  88.0f,  -50.0f, 0.20f, 0.45f, 1.00f, 1.00f,  90.0f); // blue
    add( +60.0f,  82.0f, +110.0f, 1.00f, 0.90f, 0.15f, 1.30f,  70.0f); // yellow near gate
    add(-120.0f,  95.0f,  -80.0f, 0.25f, 0.85f, 1.00f, 0.80f, 120.0f); // cyan near tree
    add(+130.0f,  80.0f,  -40.0f, 1.00f, 0.30f, 0.20f, 1.00f,  75.0f); // reddish near wall
    add( -40.0f,  92.0f, -120.0f, 0.60f, 0.20f, 1.00f, 0.90f,  85.0f); // purple
    add(+100.0f,  87.0f,  +80.0f, 0.40f, 1.00f, 0.30f, 0.70f,  65.0f); // greenish

    ERUPTION_LOG_WARN("[TEST] Appended 8 artificial point lights to test map");
}


} // namespace eruption
