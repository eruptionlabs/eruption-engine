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
            e.contour = lq.kind == 0 && !lq.triangles.empty();
            e.level = lq.level;
            e.centerX = lq.centerX;
            e.centerZ = lq.centerZ;
            e.shore = lq.shore >= 0 ? lq.shore : (lq.kind == 1 ? 1 : 0);
            // offsetX/Z/Y e rotationDeg ficam em 0: sao nudge de sessao, nao
            // fazem sentido persistir entre mapas diferentes.
            m_waterMenu.liquids.push_back(e);
        }
        // O plano de agua do mapa nao esta' em map->liquids (vem do terreno);
        // entra no fim da lista (a ordem 1:1 com map->liquids continua valendo
        // para os indices acima) para a beira dele ser editada como a das lavas.
        bool temAgua = false;
        for (const auto& e : m_waterMenu.liquids) temAgua |= e.kind == 0 && !e.contour;
        if (!temAgua && m_water.hasWater()) {
            WaterDebugMenu::LiquidEntry e;
            e.name = "Water (map plane)";
            e.kind = 0;
            e.level = m_waterMenu.config.waterLevel;
            e.shore = m_waterMenu.config.shoreSwash ? (m_waterMenu.config.shoreSwashFoam ? 2 : 1) : 0;
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
    m_contourWater.clear();
    m_shoreLavaSrc = WaterMesh{};
    m_shoreLavaCounts.clear();
    if (!map) { rebuildShoreMask(); return; }

    // Todas as lavas do mapa numa malha só, uma faixa de índices por lava
    // (antes só a primeira era desenhada). As águas de contorno (kind water com
    // polygon) vão na mesma malha, cada uma com a sua faixa: o desenho as
    // distingue por MapLiquid::kind.
    WaterMesh lmesh;
    std::vector<uint32_t> surfaceIndexCounts;

    // As do mapa aberto e as dos vizinhos em cena (já no lugar do mundo).
    std::vector<WorldLiquid> all;
    for (uint32_t i = 0; i < map->liquids.size(); ++i) all.push_back({map, i});
    for (const WorldLiquid& w : worldLiquids()) all.push_back(w);

        for (const WorldLiquid& src : all) {
            const size_t idx = src.index;
            const auto& lq = src.map->liquids[idx];
            // Água só com contorno: a sem polygon é o plano do terreno.
            if (lq.kind == 0 ? lq.triangles.empty() : (lq.kind != 1 || lq.radius <= 0.0f)) continue;
            if (m_lavaLiquids.size() == WaterRenderer::kMaxLavaSurfaces) {
                ERUPTION_LOG_WARN("Liquid surface '%s' skipped: at most %u per map", lq.name.c_str(),
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
                if (lq.kind == 0) {
                    // Caixa e área da água de contorno (nível da malha, sem o giro: o
                    // nudge do menu só mexe na lava).
                    ContourWater cw;
                    cw.drawn = uint32_t(m_lavaLiquids.size() - 1);
                    cw.mn = Vec3(1e30f);
                    cw.mx = Vec3(-1e30f);
                    double area = 0.0;
                    for (size_t t = 0; t + 2 < lq.triangles.size(); t += 3) {
                        const Vec3 &a = lq.triangles[t], &b = lq.triangles[t + 1], &c = lq.triangles[t + 2];
                        area += 0.5 * std::fabs(double(b.x - a.x) * double(c.z - a.z) - double(c.x - a.x) * double(b.z - a.z));
                        for (const Vec3* p : {&a, &b, &c}) {
                            cw.mn = glm::min(cw.mn, *p + Vec3(offX, level - lq.contourLevel, offZ));
                            cw.mx = glm::max(cw.mx, *p + Vec3(offX, level - lq.contourLevel, offZ));
                        }
                    }
                    cw.area = float(area);
                    m_contourWater.push_back(cw);
                }
                ERUPTION_LOG_INFO("%s surface '%s': outline with %zu triangle(s)", lq.kind == 0 ? "Water" : "Lava",
                                  lq.name.c_str(), lq.triangles.size() / 3);
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
    // A beira de cada lava no chao (subindo o terreno, borda incandescente)
    // usa a mesma grade da agua: remonta com as lavas novas.
    m_shoreLavaSrc = std::move(lmesh);
    m_shoreLavaCounts = std::move(surfaceIndexCounts);
    rebuildShoreMask();
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
    // Água sem contorno: o plano global (setWaterLevel).
    if (lq.kind != 1 && lq.triangles.empty()) return false;
    lq.level = level;
    return true;
}

float Engine::contourLift(uint32_t drawn) const {
    if (drawn >= m_lavaSources.size() || !m_lavaSources[drawn].map ||
        m_lavaSources[drawn].index >= m_lavaSources[drawn].map->liquids.size())
        return 0.0f;
    return m_lavaSources[drawn].map->liquids[m_lavaSources[drawn].index].level - m_lavaSources[drawn].builtLevel;
}

// Visibilidade do plano d'agua do mapa (malha do terreno), a mesma conta de
// antes das aguas de contorno.
bool Engine::waterPlaneInView() const {
    return m_water.hasWater() && !m_waterFullyBuried &&
        (!m_waterBoundsValid ||
         m_camera.frustum().intersectsAABB(
             Vec3(m_waterAabbMin.x, m_waterAabbMin.y + m_waterMenu.config.waterLevel - 2.0f, m_waterAabbMin.z),
             Vec3(m_waterAabbMax.x, m_waterAabbMax.y + m_waterMenu.config.waterLevel + 2.0f, m_waterAabbMax.z)));
}

bool Engine::contourWaterInView() const {
    for (const ContourWater& cw : m_contourWater) {
        const float lift = contourLift(cw.drawn);
        if (m_camera.frustum().intersectsAABB(cw.mn + Vec3(0.0f, lift - 2.0f, 0.0f), cw.mx + Vec3(0.0f, lift + 2.0f, 0.0f)))
            return true;
    }
    return false;
}

// A cada quadro: o plano do reflexo planar. A maior água de contorno (área) é
// a candidata; o WaterRenderer a compara com a malha do mapa (setContourPlane).
void Engine::updateContourWater() {
    float level = 0.0f, area = 0.0f;
    if (m_waterMenu.config.enabled && m_waterContourPlanar)
        for (const ContourWater& cw : m_contourWater)
            if (cw.area > area && cw.drawn < m_lavaLiquids.size()) {
                area = cw.area;
                level = m_lavaLiquids[cw.drawn].level + contourLift(cw.drawn);
            }
    m_water.setContourPlane(level, area);
}

} // namespace eruption

namespace eruption {
// Ver Engine.hpp. Agua de superficie = o plano global (o mesmo nivel que a
// malha desenha: nivel do mapa mais o ajuste do menu), dentro da malha e com o
// pe' no maximo kFolga acima da superficie. Lava nao conta; em terra seca ao
// lado da agua o piso fica acima do nivel e da' falso.
bool Engine::waterSurfaceAt(const Vec3& p, float* level) const {
    constexpr float kFolga = 0.5f;
    if (!m_currentMap) return false;
    // Água de contorno (canal, lago): a superfície sob o ponto, no nível dela.
    // Primeiro: é a mais específica, e pode estar acima do plano do mapa.
    LiquidAt contour;
    if (contourWaterAt(p.x, p.z, p.y - kFolga, contour)) {
        if (level) *level = contour.level;
        return true;
    }
    if (!m_waterOn || m_waterFullyBuried) return false;
    const float surface = m_mapBaseWaterLevel + m_waterMenu.config.waterLevel;
    if (p.y - kFolga >= surface) return false;
    if (m_waterBoundsValid &&
        (p.x < m_waterAabbMin.x || p.x > m_waterAabbMax.x || p.z < m_waterAabbMin.z || p.z > m_waterAabbMax.z))
        return false;
    if (level) *level = surface;
    return true;
}
} // namespace eruption
