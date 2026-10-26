// Líquidos do mapa: o menu de líquidos (água/lava) e o ajuste da lava ao abrir
// um mapa.
#include "core/Engine.hpp"
#include "core/EngineInternal.hpp"
#include "game/PlayerController.hpp"
#include "core/Logger.hpp"
#include "formats/MapLoader.hpp"
#include "renderer/WeatherTypes.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace eruption {

namespace {

// Malha de um liquido de contorno livre: o molde montado na carga
// (LoadedMap::buildLiquidContourMesh), girado em torno do centro e deslocado.
// O calor de margem (UV x) nao muda com giro nem deslocamento.
void appendContourMesh(const LoadedMap::MapLiquid& lq, const Vec3& move, float rotRad, WaterMesh& mesh) {
    std::shared_ptr<const WaterMesh> tpl = lq.contourMesh;
    if (!tpl) {
        LoadedMap::MapLiquid built = lq;
        LoadedMap::buildLiquidContourMesh(built);
        tpl = built.contourMesh;
        if (!tpl) return;
    }
    const float c = std::cos(rotRad), s = std::sin(rotRad);
    const uint32_t base = uint32_t(mesh.vertices.size());
    mesh.vertices.reserve(mesh.vertices.size() + tpl->vertices.size());
    for (WaterVertex v : tpl->vertices) {
        if (rotRad != 0.0f) {
            const float dx = v.position.x - lq.centerX, dz = v.position.z - lq.centerZ;
            v.position = Vec3(lq.centerX + dx * c - dz * s, v.position.y, lq.centerZ + dx * s + dz * c);
        }
        v.position += move;
        mesh.vertices.push_back(v);
    }
    mesh.indices.reserve(mesh.indices.size() + tpl->indices.size());
    for (uint32_t i : tpl->indices) mesh.indices.push_back(base + i);
    mesh.waterTileCount += tpl->waterTileCount;
}

} // namespace

void Engine::syncLiquidMenuFromMap(const LoadedMap* map) {
    m_waterMenu.liquids.clear();
    if (map) {
        for (const auto& lq : map->liquids) {
            WaterDebugMenu::LiquidEntry e;
            e.name = lq.name.empty()
                   ? (lq.kind == 1 ? "Lava" : "Water")
                   : lq.name;
            e.kind = lq.kind;
            e.level = lq.level;
            e.centerX = lq.centerX;
            e.centerZ = lq.centerZ;
            // offsetX/Z/Y e rotationDeg ficam em 0: sao nudge de sessao, nao
            // fazem sentido persistir entre mapas diferentes.
            m_waterMenu.liquids.push_back(e);
        }
    }
    m_waterMenu.ensureDefaultLiquid();
    m_waterMenu.applySelectedLiquid();
}

void Engine::setupLavaLiquid(const LoadedMap* map) {
    m_water.clearLavaMesh();
    m_lavaLiquids.clear();
    m_lavaSources.clear();
    if (!map) return;

    // Todas as lavas do mapa numa malha só, uma faixa de índices por lava
    // (antes só a primeira era desenhada).
    WaterMesh lmesh;
    std::vector<uint32_t> surfaceIndexCounts;

    // As do mapa aberto e as dos vizinhos em cena (já no lugar do mundo).
    std::vector<WorldLiquid> all;
    for (uint32_t i = 0; i < map->liquids.size(); ++i) all.push_back({map, i});
    for (const WorldLiquid& w : worldLiquids()) all.push_back(w);

        for (const WorldLiquid& src : all) {
            const size_t idx = src.index;
            const auto& lq = src.map->liquids[idx];
            if (lq.kind != 1 || lq.radius <= 0.0f) continue;
            if (m_lavaLiquids.size() == WaterRenderer::kMaxLavaSurfaces) {
                ERUPTION_LOG_WARN("Lava surface '%s' skipped: at most %u per map", lq.name.c_str(),
                                  WaterRenderer::kMaxLavaSurfaces);
                break;
            }

            // Nudge autorado no WaterDebugMenu (posicao/rotacao), por cima
            // do que o .env autorou - nunca escreve no arquivo. `idx` casa
            // 1:1 com m_waterMenu.liquids porque os dois vem da MESMA
            // ordem de map->liquids (syncLiquidMenuFromMap, chamado so' em
            // map load, nao em toda rebuild - senao um nudge em andamento
            // seria zerado no frame seguinte).
            float offX = 0.0f, offZ = 0.0f, offY = 0.0f, rotDeg = 0.0f;
            if (src.map == map && idx < m_waterMenu.liquids.size()) {
                const auto& ui = m_waterMenu.liquids[idx];
                offX = ui.offsetX; offZ = ui.offsetZ; offY = ui.offsetY; rotDeg = ui.rotationDeg;
            }
            const float centerX = lq.centerX + offX;
            const float centerZ = lq.centerZ + offZ;
            const float level = lq.level + offY;
            const float rotRad = rotDeg * 0.017453292f;

            const size_t firstIndex = lmesh.indices.size();
            if (!lq.triangles.empty()) {
                appendContourMesh(lq, Vec3(offX, offY + lq.level - lq.contourLevel, offZ), rotRad, lmesh);
                surfaceIndexCounts.push_back(uint32_t(lmesh.indices.size() - firstIndex));
                LoadedMap::MapLiquid drawn = lq;
                drawn.centerX = centerX;
                drawn.centerZ = centerZ;
                drawn.level = level;
                m_lavaLiquids.push_back(drawn);
                m_lavaSources.push_back({src.map, uint32_t(idx), lq.level});
                ERUPTION_LOG_INFO("Lava surface '%s': outline with %zu triangle(s)", lq.name.c_str(),
                                  lq.triangles.size() / 3);
                continue;
            }
            const uint32_t base = uint32_t(lmesh.vertices.size());
            const int rings = 24;
            const int segs = 64;
            // Margem LOBADA: se o mapa autorou shoreRadius, o raio MAXIMO
            // pra cada segmento angular vem de la' (interpolado, wraparound
            // incluido) em vez do escalar `radius` constante. Um circulo nao
            // acompanha uma bacia de margem lobada - sobra vazio nos lobos largos ou
            // vaza pra fora nos estreitos, nao tem raio unico que resolva os
            // dois ao mesmo tempo. shoreRadius vazio (todo mapa anterior a
            // essa feature) cai exatamente no caminho de antes.
            const size_t shoreN = lq.shoreRadius.size();
            auto radiusAt = [&](int a) -> float {
                if (shoreN == 0) return lq.radius;
                const float f = (float(a) / float(segs)) * float(shoreN);
                const size_t i0 = static_cast<size_t>(f) % shoreN;
                const size_t i1 = (i0 + 1) % shoreN;
                const float t = f - std::floor(f);
                return lq.shoreRadius[i0] * (1.0f - t) + lq.shoreRadius[i1] * t;
            };
            WaterVertex wv{};
            wv.position = Vec3(centerX, level, centerZ);
            // A lava nao usa UV (a cor e' de mundo): o x leva a FRACAO RADIAL
            // (0 no centro, 1 na margem) para o calor de borda (shoreHeat /
            // centerHeat do .env).
            wv.texCoord = Vec2(0.0f, 0.0f);
            lmesh.vertices.push_back(wv);
            for (int r = 1; r <= rings; ++r) {
                for (int a = 0; a < segs; ++a) {
                    // rotRad gira o PERFIL (a rotula qual amostra de
                    // shoreRadius fica em qual angulo do mundo), nao a
                    // amostragem em si - assim o lobo gira em volta do
                    // centro em vez de so' deslocar a fase da textura.
                    const float th = 6.2831853f * float(a) / float(segs) + rotRad;
                    const float rr = radiusAt(a) * (float(r) / float(rings));
                    const float px = centerX + rr * std::cos(th);
                    const float pz = centerZ + rr * std::sin(th);
                    WaterVertex rv{};
                    rv.position = Vec3(px, level, pz);
                    rv.texCoord = Vec2(float(r) / float(rings), 0.0f);
                    lmesh.vertices.push_back(rv);
                }
            }
            auto ringIdx = [&](int r, int a) {
                return base + uint32_t(1 + (r - 1) * segs + (a % segs));
            };
            for (int a = 0; a < segs; ++a) {
                lmesh.indices.push_back(base);
                lmesh.indices.push_back(ringIdx(1, a));
                lmesh.indices.push_back(ringIdx(1, a + 1));
            }
            for (int r = 1; r < rings; ++r) {
                for (int a = 0; a < segs; ++a) {
                    lmesh.indices.push_back(ringIdx(r, a));
                    lmesh.indices.push_back(ringIdx(r + 1, a));
                    lmesh.indices.push_back(ringIdx(r + 1, a + 1));
                    lmesh.indices.push_back(ringIdx(r, a));
                    lmesh.indices.push_back(ringIdx(r + 1, a + 1));
                    lmesh.indices.push_back(ringIdx(r, a + 1));
                }
            }
            lmesh.waterTileCount += uint32_t(rings * segs);
            surfaceIndexCounts.push_back(uint32_t(lmesh.indices.size() - firstIndex));
            LoadedMap::MapLiquid drawn = lq;
            drawn.centerX = centerX;
            drawn.centerZ = centerZ;
            drawn.level = level;
            m_lavaLiquids.push_back(drawn);
            m_lavaSources.push_back({src.map, uint32_t(idx), lq.level});
            ERUPTION_LOG_WARN("Lava surface: level=%.1f center=(%.0f,%.0f) r=%.0f",
                              level, centerX, centerZ, lq.radius);
        }
    if (!m_lavaLiquids.empty()) m_water.setLavaMesh(lmesh, surfaceIndexCounts);
}

// Lava subindo durante o jogo: muda só o nível do líquido do mapa. A malha
// não é refeita (setLavaMesh espera a GPU); o Render passa a diferença para o
// shader como deslocamento em Y (levelOffset), e as consultas do chão leem
// map->liquids direto.
bool Engine::setLiquidLevel(int index, float level, LoadedMap* map) {
    if (!map) map = m_currentMap.get();
    if (!map || index < 0 || size_t(index) >= map->liquids.size() || !std::isfinite(level))
        return false;
    LoadedMap::MapLiquid& lq = map->liquids[size_t(index)];
    if (lq.kind != 1) return false; // água: o plano global (setWaterLevel)
    lq.level = level;
    return true;
}

} // namespace eruption
