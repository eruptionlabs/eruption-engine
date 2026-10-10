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
    m_lavaLiquid = LoadedMap::MapLiquid{};
    m_hasLavaLiquid = false;
    if (!map) return;

        for (size_t idx = 0; idx < map->liquids.size(); ++idx) {
            const auto& lq = map->liquids[idx];
            if (lq.kind != 1 || lq.radius <= 0.0f) continue;

            // Nudge autorado no WaterDebugMenu (posicao/rotacao), por cima
            // do que o .env autorou - nunca escreve no arquivo. `idx` casa
            // 1:1 com m_waterMenu.liquids porque os dois vem da MESMA
            // ordem de map->liquids (syncLiquidMenuFromMap, chamado so' em
            // map load, nao em toda rebuild - senao um nudge em andamento
            // seria zerado no frame seguinte).
            float offX = 0.0f, offZ = 0.0f, offY = 0.0f, rotDeg = 0.0f;
            if (idx < m_waterMenu.liquids.size()) {
                const auto& ui = m_waterMenu.liquids[idx];
                offX = ui.offsetX; offZ = ui.offsetZ; offY = ui.offsetY; rotDeg = ui.rotationDeg;
            }
            const float centerX = lq.centerX + offX;
            const float centerZ = lq.centerZ + offZ;
            const float level = lq.level + offY;
            const float rotRad = rotDeg * 0.017453292f;

            WaterMesh lmesh;
            const int rings = 24;
            const int segs = 64;
            // Margem LOBADA: se o mapa autorou shoreRadius, o raio MAXIMO
            // pra cada segmento angular vem de la' (interpolado, wraparound
            // incluido) em vez do escalar `radius` constante. Um circulo nao
            // acompanha uma caldeira lobada - sobra vazio nos lobos largos ou
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
            wv.texCoord = Vec2(0.5f, 0.5f);
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
                    rv.texCoord = Vec2(px / 90.0f, pz / 90.0f);
                    lmesh.vertices.push_back(rv);
                }
            }
            auto ringIdx = [&](int r, int a) {
                return uint32_t(1 + (r - 1) * segs + (a % segs));
            };
            for (int a = 0; a < segs; ++a) {
                lmesh.indices.push_back(0);
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
            lmesh.waterTileCount = uint32_t(rings * segs);
            m_water.setLavaMesh(lmesh);
            m_lavaLiquid = lq;
            m_lavaLiquid.centerX = centerX;
            m_lavaLiquid.centerZ = centerZ;
            m_lavaLiquid.level = level;
            m_hasLavaLiquid = true;
            ERUPTION_LOG_WARN("Lava surface: level=%.1f center=(%.0f,%.0f) r=%.0f",
                              level, centerX, centerZ, lq.radius);
            break;
        }
}

} // namespace eruption
