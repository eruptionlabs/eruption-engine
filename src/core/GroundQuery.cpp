// Consultas do chão para o jogo: altura do piso (terreno ou topo de objeto
// sólido), se dá para ficar de pé ali, que líquido aparece no ponto, e as três
// de uma vez numa grade. O script lia ponto a ponto (world.ground_height e
// world.walkable): ~80 mil chamadas estouravam o orçamento de 250 ms de uma
// chamada de script, e a altura ignorava as peças de piso dos mapas de kit
// (0 embaixo de uma passarela a 48).
#include "core/Engine.hpp"
#include "formats/MapLoader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace eruption {

namespace {

constexpr float kBody = 20.0f; // altura do corpo, a mesma de Engine::blockedAt
// Sólido que não passa disto acima do piso não bloqueia: é a peça vizinha da
// mesma laje ou uma borda baixa, não parede.
constexpr float kStep = 4.0f;
// De cima de tudo: entre mapas empilhados, o chão mais alto.
constexpr float kTop = std::numeric_limits<float>::max();

struct Floor {
    float y = 0.0f;
    int instance = -1; // -1 = terreno
};

bool insideXZ(const ModelInstance& inst, float x, float z, float grow) {
    return x >= inst.worldAabbMin.x - grow && x <= inst.worldAabbMax.x + grow &&
           z >= inst.worldAabbMin.z - grow && z <= inst.worldAabbMax.z + grow;
}

// Sólidos ligados cuja caixa cruza o retângulo [x0, x1] x [z0, z1].
void solidsIn(const std::vector<uint8_t>& solid, const std::vector<ModelInstance>& insts, float x0, float z0,
              float x1, float z1, std::vector<uint32_t>& out) {
    out.clear();
    for (uint32_t i = 0; i < solid.size() && i < insts.size(); ++i) {
        if (!solid[i] || !insts[i].enabled) continue;
        const ModelInstance& inst = insts[i];
        if (inst.worldAabbMax.x < x0 || inst.worldAabbMin.x > x1 || inst.worldAabbMax.z < z0 ||
            inst.worldAabbMin.z > z1)
            continue;
        out.push_back(i);
    }
}

// Raio vertical de cima para baixo nos sólidos: o topo mais alto abaixo de
// fromY que fique acima do terreno.
Floor floorOver(const ModelRenderer& mr, const std::vector<uint32_t>& cand, float terrainY, float x, float z,
                float fromY) {
    Floor f{terrainY, -1};
    const auto& insts = mr.getInstances();
    for (uint32_t i : cand) {
        const ModelInstance& inst = insts[i];
        if (!insideXZ(inst, x, z, 0.0f) || inst.worldAabbMin.y >= fromY) continue;
        const float top = std::min(fromY, inst.worldAabbMax.y + 1.0f);
        if (top <= f.y) continue;
        const float t = mr.raycastInstance(i, Vec3(x, top, z), Vec3(0.0f, -1.0f, 0.0f));
        if (t < 0.0f || top - t <= f.y) continue;
        f = {top - t, int(i)};
    }
    return f;
}

// Raio da margem no ângulo de (dx, dz): a margem lobada (amostras por ângulo,
// a mesma interpolação de Engine::setupLavaLiquid) ou o raio fixo.
float shoreAt(const LoadedMap::MapLiquid& lq, float dx, float dz) {
    const size_t n = lq.shoreRadius.size();
    if (n == 0) return lq.radius;
    float th = std::atan2(dz, dx);
    if (th < 0.0f) th += 6.2831853f;
    const float f = th / 6.2831853f * float(n);
    const size_t i0 = size_t(f) % n, i1 = (i0 + 1) % n;
    const float t = f - std::floor(f);
    return lq.shoreRadius[i0] * (1.0f - t) + lq.shoreRadius[i1] * t;
}

// Superfície de um líquido de contorno livre em (x, z): a altura do
// triângulo que contém o ponto (mais o quanto o nível vivo andou desde a
// carga), ou false fora do contorno.
bool contourSurfaceAt(const LoadedMap::MapLiquid& lq, float x, float z, float& y) {
    const float dx = x - lq.centerX, dz = z - lq.centerZ;
    if (dx * dx + dz * dz > lq.radius * lq.radius) return false;
    for (size_t t = 0; t + 2 < lq.triangles.size(); t += 3) {
        const Vec3 &a = lq.triangles[t], &b = lq.triangles[t + 1], &c = lq.triangles[t + 2];
        const float den = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (std::fabs(den) < 1e-12f) continue;
        const float wa = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / den;
        const float wb = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / den;
        const float wc = 1.0f - wa - wb;
        if (wa < -1e-5f || wb < -1e-5f || wc < -1e-5f) continue;
        y = wa * a.y + wb * b.y + wc * c.y + (lq.level - lq.contourLevel);
        return true;
    }
    return false;
}

// Índice do líquido de lava aparente em (x, z) com o piso na altura y, ou -1;
// `surface` recebe a altura da lava ali.
int lavaAt(const LoadedMap* map, float x, float z, float y, float& surface) {
    if (!map) return -1;
    for (size_t i = 0; i < map->liquids.size(); ++i) {
        const auto& lq = map->liquids[i];
        if (lq.kind != 1) continue;
        if (!lq.triangles.empty()) {
            if (contourSurfaceAt(lq, x, z, surface) && y < surface) return int(i);
            continue;
        }
        if (y >= lq.level) continue;
        const float dx = x - lq.centerX, dz = z - lq.centerZ;
        const float r = shoreAt(lq, dx, dz);
        if (r > 0.0f && dx * dx + dz * dz <= r * r) {
            surface = lq.level;
            return int(i);
        }
    }
    return -1;
}

} // namespace

// Algum sólido além do próprio piso ocupa o corpo acima dele, no raio?
bool Engine::bodyBlocked(const std::vector<uint32_t>& cand, int floorInstance, float floorY, float x, float z,
                         float radius) const {
    const auto& insts = m_modelRenderer.getInstances();
    for (uint32_t i : cand) {
        if (int(i) == floorInstance) continue;
        const ModelInstance& inst = insts[i];
        if (inst.worldAabbMax.y < floorY + kStep || inst.worldAabbMin.y > floorY + kBody) continue;
        if (insideXZ(inst, x, z, radius) && footprintContains(i, Vec2(x, z), radius)) return true;
    }
    return false;
}

float Engine::floorHeight(float x, float z, float fromY, bool* onModel) const {
    std::vector<uint32_t> cand;
    solidsIn(m_solid, m_modelRenderer.getInstances(), x, z, x, z, cand);
    const Floor f = floorOver(m_modelRenderer, cand, sampleTerrainHeight(x, z, fromY), x, z, fromY);
    if (onModel) *onModel = f.instance >= 0;
    return f.y;
}

// Líquido sobre o piso na altura floorY em (x, z). Lava antes da água: o
// plano de água é global e a lava fica numa bacia.
bool Engine::liquidOver(float x, float z, float floorY, LiquidAt& out) const {
    float surface = 0.0f;
    const int lava = lavaAt(m_currentMap.get(), x, z, floorY, surface);
    if (lava >= 0) {
        out = {lava, 1, surface};
        return true;
    }
    if (m_waterOn && floorY < m_mapBaseWaterLevel) {
        out = {-1, 0, m_mapBaseWaterLevel};
        return true;
    }
    return false;
}

// Grade (no terreno) ou a peça (sobre sólido), corpo livre e no máximo um
// degrau de líquido sobre o piso: a grade de caminhada não sabe do líquido
// (no teste do jogo, piso a -133 com lava a -117 saía andável).
bool Engine::walkableOver(const std::vector<uint32_t>& cand, int floorInstance, float floorY, float x, float z,
                          float radius) const {
    if (floorInstance < 0 && !navWalkable(x, z, floorY)) return false;
    LiquidAt lq;
    if (liquidOver(x, z, floorY, lq) && lq.level - floorY > kStep) return false;
    return !bodyBlocked(cand, floorInstance, floorY, x, z, radius);
}

bool Engine::floorWalkable(float x, float z, float radius, float fromY) const {
    std::vector<uint32_t> cand;
    solidsIn(m_solid, m_modelRenderer.getInstances(), x - radius, z - radius, x + radius, z + radius, cand);
    const Floor f = floorOver(m_modelRenderer, cand, sampleTerrainHeight(x, z, fromY), x, z, fromY);
    return walkableOver(cand, f.instance, f.y, x, z, radius);
}

bool Engine::liquidAt(float x, float z, LiquidAt& out) const {
    std::vector<uint32_t> cand;
    solidsIn(m_solid, m_modelRenderer.getInstances(), x, z, x, z, cand);
    const float y = floorOver(m_modelRenderer, cand, sampleTerrainHeight(x, z, kTop), x, z,
                              std::numeric_limits<float>::max()).y;
    return liquidOver(x, z, y, out);
}

void Engine::sampleGround(float x0, float z0, float step, int nx, int nz, float radius, float* heights,
                          uint8_t* flags) const {
    if (nx <= 0 || nz <= 0) return;
    const auto& insts = m_modelRenderer.getInstances();
    const float x1 = x0 + step * float(nx - 1);
    std::vector<uint32_t> row, cand;
    for (int iz = 0; iz < nz; ++iz) {
        const float z = z0 + step * float(iz);
        // Sólidos desta linha (com o raio): o teste por ponto só vê esses.
        solidsIn(m_solid, insts, std::min(x0, x1) - radius, z - radius, std::max(x0, x1) + radius, z + radius, row);
        for (int ix = 0; ix < nx; ++ix) {
            const float x = x0 + step * float(ix);
            cand.clear();
            for (uint32_t i : row)
                if (insideXZ(insts[i], x, z, radius)) cand.push_back(i);
            const Floor f = floorOver(m_modelRenderer, cand, sampleTerrainHeight(x, z, kTop), x, z,
                                      std::numeric_limits<float>::max());
            uint8_t fl = f.instance >= 0 ? kGroundOnModel : 0;
            if (walkableOver(cand, f.instance, f.y, x, z, radius)) fl |= kGroundWalkable;
            LiquidAt lq;
            if (liquidOver(x, z, f.y, lq)) fl |= lq.kind == 1 ? kGroundLava : kGroundWater;
            const size_t o = size_t(iz) * size_t(nx) + size_t(ix);
            heights[o] = f.y;
            flags[o] = fl;
        }
    }
}

} // namespace eruption
