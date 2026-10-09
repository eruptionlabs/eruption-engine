// Retoque de terreno: uma grade de deltas de altura por cima da grade do mapa
// (.ter), guardada à parte em assets/scenes/<mapa>.height.bin. Na carga o
// delta entra nos vértices do chão alinhados à grade e na grade .ter (jogo e
// editor). No editor o pincel muda o delta e envia só os vértices tocados.
#include "core/Engine.hpp"

#include "core/EngineInternal.hpp"
#include "core/Logger.hpp"
#include "formats/MapLoader.hpp"
#include "scene/SceneEdits.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace eruption {

namespace {

namespace fs = std::filesystem;

constexpr float kAlignTol = 1e-3f;  // vértice "em cima" de um ponto da grade
constexpr float kHeightTol = 0.05f; // e na altura do chão nesse ponto

fs::path heightPathFor(const std::string& map) {
    fs::path p = SceneEdits::pathFor(map);
    p.replace_extension(); // tira .luau
    p.replace_extension(".height.bin");
    return p;
}

// Ponto da grade sob (x, z) em mundo, se o vértice cai exatamente nele.
bool gridPoint(const Engine::HeightEdit& h, float x, float z, uint32_t& g) {
    const float gx = (x - h.offX) / h.scale, gz = (z - h.offZ) / h.scale;
    const float rx = std::round(gx), rz = std::round(gz);
    if (std::abs(gx - rx) > kAlignTol || std::abs(gz - rz) > kAlignTol) return false;
    if (rx < 0 || rz < 0 || rx >= float(h.w) || rz >= float(h.d)) return false;
    g = static_cast<uint32_t>(rz) * h.w + static_cast<uint32_t>(rx);
    return true;
}

Vec3 gridNormal(const Engine::HeightEdit& h, uint32_t x, uint32_t z) {
    const uint32_t xl = x > 0 ? x - 1 : x, xr = std::min(x + 1, h.w - 1);
    const uint32_t zd = z > 0 ? z - 1 : z, zu = std::min(z + 1, h.d - 1);
    const float dx = (h.height(z * h.w + xr) - h.height(z * h.w + xl)) / (float(xr - xl) * h.scale);
    const float dz = (h.height(zu * h.w + x) - h.height(zd * h.w + x)) / (float(zu - zd) * h.scale);
    return glm::normalize(Vec3(-dx, 1.0f, -dz));
}

} // namespace

void Engine::prepareHeightEdits(LoadedMap& map, std::vector<ModelAsset>& models) {
    HeightEdit& h = m_heightEdit;
    h = HeightEdit{};
    if (!map.terrainFromFile || map.models.empty()) return;
    const TerrainFile& ter = map.terrain;
    h.w = ter.width;
    h.d = ter.height;
    h.scale = ter.scale > 0.0f ? ter.scale : 10.0f;
    h.offX = ter.offsetX;
    h.offZ = ter.offsetZ;
    h.assetPath = map.models.front().filePath;
    h.mapName = map.mapName;
    h.base.resize(size_t(h.w) * h.d);
    for (size_t g = 0; g < h.base.size(); ++g) h.base[g] = -ter.cubes[g].height[0];
    h.delta.assign(h.base.size(), 0.0f);
    h.active = true;

    // Retoque salvo?
    std::ifstream f(heightPathFor(map.mapName), std::ios::binary);
    if (!f) return;
    char magic[8] = {};
    uint32_t version = 0, w = 0, d = 0;
    float scale = 0, ox = 0, oz = 0;
    f.read(magic, 8);
    f.read(reinterpret_cast<char*>(&version), 4);
    f.read(reinterpret_cast<char*>(&w), 4);
    f.read(reinterpret_cast<char*>(&d), 4);
    f.read(reinterpret_cast<char*>(&scale), 4);
    f.read(reinterpret_cast<char*>(&ox), 4);
    f.read(reinterpret_cast<char*>(&oz), 4);
    if (std::memcmp(magic, "ERUPTHGT", 8) != 0 || version != 1 || w != h.w || d != h.d || scale != h.scale) {
        ERUPTION_LOG_WARN("Terrain edits for %s don't match the map grid; ignored", map.mapName.c_str());
        return;
    }
    f.read(reinterpret_cast<char*>(h.delta.data()), static_cast<std::streamsize>(h.delta.size() * sizeof(float)));
    if (!f) {
        h.delta.assign(h.base.size(), 0.0f);
        return;
    }

    // Grade .ter (altura do jogador, raios) e vértices do chão antes de subir.
    for (uint32_t z = 0; z < h.d; ++z)
        for (uint32_t x = 0; x < h.w; ++x) {
            TerrainCube& c = map.terrain.cubes[size_t(z) * h.w + x];
            const uint32_t x1 = std::min(x + 1, h.w - 1), z1 = std::min(z + 1, h.d - 1);
            c.height[0] = -h.height(z * h.w + x);
            c.height[1] = -h.height(z * h.w + x1);
            c.height[2] = -h.height(z1 * h.w + x);
            c.height[3] = -h.height(z1 * h.w + x1);
        }
    size_t moved = 0;
    for (auto& asset : models) {
        if (asset.filePath != h.assetPath) continue;
        for (auto& node : asset.nodes) {
            const Mat4& m = node.renderMatrix;
            const Mat4 inv = glm::inverse(m);
            const glm::mat3 toLocalN = glm::transpose(glm::mat3(m));
            for (auto& v : node.vertices) {
                const Vec3 wp = Vec3(m * Vec4(v.position, 1.0f));
                uint32_t g = 0;
                if (!gridPoint(h, wp.x, wp.z, g) || std::abs(wp.y - h.base[g]) > kHeightTol) continue;
                const uint32_t gx = g % h.w, gz = g / h.w;
                // Só onde há retoque (no ponto ou num vizinho, que muda a normal).
                bool touched = false;
                for (int dz = -1; dz <= 1 && !touched; ++dz)
                    for (int dx = -1; dx <= 1 && !touched; ++dx) {
                        const int nx = int(gx) + dx, nz = int(gz) + dz;
                        if (nx >= 0 && nz >= 0 && nx < int(h.w) && nz < int(h.d) && h.delta[size_t(nz) * h.w + nx] != 0.0f)
                            touched = true;
                    }
                if (!touched) continue;
                v.position = Vec3(inv * Vec4(wp.x, h.height(g), wp.z, 1.0f));
                v.normal = glm::normalize(toLocalN * gridNormal(h, gx, gz));
                ++moved;
            }
        }
    }
    ERUPTION_LOG_WARN("Terrain edits: %zu ground vertices moved (%s)", moved, heightPathFor(map.mapName).string().c_str());
}

void Engine::bindHeightEditToMeshes() {
    HeightEdit& h = m_heightEdit;
    h.gpuStart.clear();
    h.gpuRefs.clear();
    h.cpuStart.clear();
    h.cpuRefs.clear();
    h.meshTransform.clear();
    if (!h.active) return;
    const size_t n = h.base.size();
    // CSR: primeiro conta por ponto, depois preenche.
    auto build = [&](auto&& forEach, std::vector<uint32_t>& start, std::vector<HeightEdit::Ref>& refs) {
        std::vector<uint32_t> count(n + 1, 0);
        forEach([&](uint32_t g, uint32_t, uint32_t) { ++count[g + 1]; });
        for (size_t i = 0; i < n; ++i) count[i + 1] += count[i];
        start = count;
        refs.resize(count[n]);
        std::vector<uint32_t> fill(count.begin(), count.end() - 1);
        forEach([&](uint32_t g, uint32_t a, uint32_t b) { refs[fill[g]++] = {a, b}; });
    };
    ModelRenderer& mr = m_modelRenderer;
    for (uint32_t mesh : mr.groundMeshes())
        for (const auto& inst : mr.getInstances())
            if (inst.meshIndex == mesh) { h.meshTransform[mesh] = inst.transform; break; }
    build([&](auto&& emit) {
              for (const auto& [mesh, t] : h.meshTransform) {
                  const auto* verts = mr.groundVertices(mesh);
                  for (uint32_t i = 0; verts && i < verts->size(); ++i) {
                      const Vec3 wp = Vec3(t * Vec4((*verts)[i].position, 1.0f));
                      uint32_t g = 0;
                      if (gridPoint(h, wp.x, wp.z, g) && std::abs(wp.y - h.height(g)) <= kHeightTol) emit(g, mesh, i);
                  }
              }
          },
          h.gpuStart, h.gpuRefs);
    if (ModelAsset* asset = mr.assetFor(h.assetPath)) {
        build([&](auto&& emit) {
                  for (uint32_t nIdx = 0; nIdx < asset->nodes.size(); ++nIdx) {
                      const auto& node = asset->nodes[nIdx];
                      for (uint32_t i = 0; i < node.vertices.size(); ++i) {
                          const Vec3 wp = Vec3(node.renderMatrix * Vec4(node.vertices[i].position, 1.0f));
                          uint32_t g = 0;
                          if (gridPoint(h, wp.x, wp.z, g) && std::abs(wp.y - h.height(g)) <= kHeightTol) emit(g, nIdx, i);
                      }
                  }
              },
              h.cpuStart, h.cpuRefs);
    }
    ERUPTION_LOG_WARN("Terrain editing: %zux%zu grid, %zu GPU / %zu CPU ground vertices", size_t(h.w), size_t(h.d),
                      h.gpuRefs.size(), h.cpuRefs.size());
}

bool Engine::terrainEditable() const { return m_heightEdit.active && !m_heightEdit.gpuRefs.empty(); }

void Engine::refreshHeightRegion(int x0, int z0, int x1, int z1) {
    HeightEdit& h = m_heightEdit;
    if (!h.active || !m_currentMap) return;
    // A normal de um ponto depende dos vizinhos: atualiza uma borda a mais.
    x0 = std::max(0, x0 - 1);
    z0 = std::max(0, z0 - 1);
    x1 = std::min(int(h.w) - 1, x1 + 1);
    z1 = std::min(int(h.d) - 1, z1 + 1);
    auto& cubes = m_currentMap->terrain.cubes;
    for (int z = std::max(0, z0 - 1); z <= z1; ++z)
        for (int x = std::max(0, x0 - 1); x <= x1; ++x) {
            TerrainCube& c = cubes[size_t(z) * h.w + x];
            const uint32_t xa = uint32_t(x), za = uint32_t(z);
            const uint32_t xb = std::min(xa + 1, h.w - 1), zb = std::min(za + 1, h.d - 1);
            c.height[0] = -h.height(za * h.w + xa);
            c.height[1] = -h.height(za * h.w + xb);
            c.height[2] = -h.height(zb * h.w + xa);
            c.height[3] = -h.height(zb * h.w + xb);
        }
    ModelRenderer& mr = m_modelRenderer;
    std::unordered_map<uint32_t, std::vector<uint32_t>> patches;
    std::unordered_map<uint32_t, std::pair<Vec3, Vec3>> bounds;
    ModelAsset* asset = mr.assetFor(h.assetPath);
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const uint32_t g = uint32_t(z) * h.w + uint32_t(x);
            const Vec3 n = gridNormal(h, uint32_t(x), uint32_t(z));
            const float wx = h.offX + x * h.scale, wz = h.offZ + z * h.scale;
            for (uint32_t r = h.gpuStart[g]; r < h.gpuStart[g + 1]; ++r) {
                const auto& ref = h.gpuRefs[r];
                const Mat4& t = h.meshTransform[ref.a];
                auto& v = (*mr.groundVertices(ref.a))[ref.b];
                v.position = Vec3(glm::inverse(t) * Vec4(wx, h.height(g), wz, 1.0f));
                v.normal = glm::normalize(glm::transpose(glm::mat3(t)) * n);
                patches[ref.a].push_back(ref.b);
                auto [it, fresh] = bounds.try_emplace(ref.a, v.position, v.position);
                it->second.first = glm::min(it->second.first, v.position);
                it->second.second = glm::max(it->second.second, v.position);
            }
            if (!asset || h.cpuStart.empty()) continue;
            for (uint32_t r = h.cpuStart[g]; r < h.cpuStart[g + 1]; ++r) {
                const auto& ref = h.cpuRefs[r];
                auto& node = asset->nodes[ref.a];
                auto& v = node.vertices[ref.b];
                v.position = Vec3(glm::inverse(node.renderMatrix) * Vec4(wx, h.height(g), wz, 1.0f));
                v.normal = glm::normalize(glm::transpose(glm::mat3(node.renderMatrix)) * n);
            }
        }
    for (auto& [mesh, list] : patches) mr.patchGroundVertices(mesh, list);
    for (auto& [mesh, b] : bounds) mr.growMeshBounds(mesh, b.first, b.second);
}

// Altura da grade (bilinear) usando `old` dentro da região salva.
float Engine::gridHeightAt(float wx, float wz, int x0, int z0, int x1, int z1, const std::vector<float>* old) const {
    const HeightEdit& h = m_heightEdit;
    const float gx = std::clamp((wx - h.offX) / h.scale, 0.0f, float(h.w - 1));
    const float gz = std::clamp((wz - h.offZ) / h.scale, 0.0f, float(h.d - 1));
    const int ix = std::min(int(gx), int(h.w) - 2 < 0 ? 0 : int(h.w) - 2);
    const int iz = std::min(int(gz), int(h.d) - 2 < 0 ? 0 : int(h.d) - 2);
    auto at = [&](int x, int z) {
        x = std::clamp(x, 0, int(h.w) - 1);
        z = std::clamp(z, 0, int(h.d) - 1);
        if (old && x >= x0 && x <= x1 && z >= z0 && z <= z1) return (*old)[size_t(z - z0) * (x1 - x0 + 1) + (x - x0)];
        return h.height(uint32_t(z) * h.w + uint32_t(x));
    };
    const float fx = gx - ix, fz = gz - iz;
    return at(ix, iz) * (1 - fx) * (1 - fz) + at(ix + 1, iz) * fx * (1 - fz) + at(ix, iz + 1) * (1 - fx) * fz +
           at(ix + 1, iz + 1) * fx * fz;
}

void Engine::reprojectOnTerrain(int x0, int z0, int x1, int z1, const std::vector<float>& oldHeights) {
    const HeightEdit& h = m_heightEdit;
    const float minX = h.offX + (x0 - 1) * h.scale, maxX = h.offX + (x1 + 1) * h.scale;
    const float minZ = h.offZ + (z0 - 1) * h.scale, maxZ = h.offZ + (z1 + 1) * h.scale;
    // Objetos e personagens em cima do chão acompanham a mudança de altura
    // no ponto de origem deles (grama não enterra, árvore não flutua).
    std::vector<std::pair<uint32_t, float>> moves;
    const auto& insts = m_modelRenderer.getInstances();
    for (uint32_t i = 0; i < insts.size(); ++i) {
        const ModelInstance& inst = insts[i];
        if (m_modelRenderer.isGroundMesh(inst.meshIndex)) continue;
        const float px = inst.transform[3].x, pz = inst.transform[3].z;
        if (px < minX || px > maxX || pz < minZ || pz > maxZ) continue;
        const float dy = gridHeightAt(px, pz, x0, z0, x1, z1, nullptr) - gridHeightAt(px, pz, x0, z0, x1, z1, &oldHeights);
        if (std::abs(dy) > 1e-5f) moves.push_back({i, dy});
    }
    m_modelRenderer.offsetInstancesY(moves);
    for (auto& c : m_characters) {
        if (c.position.x < minX || c.position.x > maxX || c.position.z < minZ || c.position.z > maxZ) continue;
        c.position.y += gridHeightAt(c.position.x, c.position.z, x0, z0, x1, z1, nullptr) -
                        gridHeightAt(c.position.x, c.position.z, x0, z0, x1, z1, &oldHeights);
    }
}

bool Engine::sculptTerrain(SculptMode mode, const Vec2& center, float radius, float strength, float falloff,
                           float flattenTo, float dt, int rect[4]) {
    HeightEdit& h = m_heightEdit;
    if (!terrainEditable() || radius <= 0.0f) return false;
    const int x0 = std::max(0, int(std::floor((center.x - radius - h.offX) / h.scale)));
    const int z0 = std::max(0, int(std::floor((center.y - radius - h.offZ) / h.scale)));
    const int x1 = std::min(int(h.w) - 1, int(std::ceil((center.x + radius - h.offX) / h.scale)));
    const int z1 = std::min(int(h.d) - 1, int(std::ceil((center.y + radius - h.offZ) / h.scale)));
    if (x0 > x1 || z0 > z1) return false;
    const float inner = std::clamp(1.0f - falloff, 0.0f, 1.0f);
    std::vector<float> before;
    before.reserve(size_t(x1 - x0 + 1) * (z1 - z0 + 1));
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) before.push_back(h.height(uint32_t(z) * h.w + uint32_t(x)));
    const bool smoothing = mode == SculptMode::Smooth;
    auto heightAt = [&](int x, int z) {
        x = std::clamp(x, 0, int(h.w) - 1);
        z = std::clamp(z, 0, int(h.d) - 1);
        if (smoothing && x >= x0 && x <= x1 && z >= z0 && z <= z1)
            return before[size_t(z - z0) * (x1 - x0 + 1) + (x - x0)];
        return h.height(uint32_t(z) * h.w + uint32_t(x));
    };
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const float wx = h.offX + x * h.scale, wz = h.offZ + z * h.scale;
            const float t = glm::length(Vec2(wx, wz) - center) / radius;
            if (t >= 1.0f) continue;
            const float w = t <= inner ? 1.0f : 1.0f - glm::smoothstep(inner, 1.0f, t);
            const uint32_t g = uint32_t(z) * h.w + uint32_t(x);
            const float cur = h.height(g);
            switch (mode) {
                case SculptMode::Raise: h.delta[g] += strength * w * dt; break;
                case SculptMode::Lower: h.delta[g] -= strength * w * dt; break;
                case SculptMode::Smooth: {
                    const float avg = (heightAt(x - 1, z) + heightAt(x + 1, z) + heightAt(x, z - 1) + heightAt(x, z + 1)) * 0.25f;
                    h.delta[g] += (avg - cur) * std::min(1.0f, strength * 0.05f * w * dt);
                    break;
                }
                case SculptMode::Flatten:
                    h.delta[g] += (flattenTo - cur) * std::min(1.0f, strength * 0.05f * w * dt);
                    break;
            }
        }
    refreshHeightRegion(x0, z0, x1, z1);
    reprojectOnTerrain(x0, z0, x1, z1, before);
    rect[0] = x0;
    rect[1] = z0;
    rect[2] = x1;
    rect[3] = z1;
    return true;
}

void Engine::setHeightDeltaRegion(int x0, int z0, int x1, int z1, const std::vector<float>& values) {
    HeightEdit& h = m_heightEdit;
    if (!h.active) return;
    std::vector<float> before;
    before.reserve(values.size());
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) before.push_back(h.height(uint32_t(z) * h.w + uint32_t(x)));
    size_t k = 0;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) h.delta[size_t(z) * h.w + x] = values[k++];
    refreshHeightRegion(x0, z0, x1, z1);
    reprojectOnTerrain(x0, z0, x1, z1, before);
}

std::vector<float> Engine::heightDeltaRegion(int x0, int z0, int x1, int z1) const {
    const HeightEdit& h = m_heightEdit;
    std::vector<float> out;
    out.reserve(size_t(x1 - x0 + 1) * (z1 - z0 + 1));
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) out.push_back(h.delta[size_t(z) * h.w + x]);
    return out;
}

bool Engine::saveHeightDelta(std::string& error) const {
    const HeightEdit& h = m_heightEdit;
    if (!h.active) return true;
    const fs::path path = heightPathFor(h.mapName.empty() ? m_currentMapName : h.mapName);
    const bool any = std::any_of(h.delta.begin(), h.delta.end(), [](float v) { return v != 0.0f; });
    std::error_code ec;
    if (!any) {
        fs::remove(path, ec);
        return true;
    }
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    const uint32_t version = 1;
    f.write("ERUPTHGT", 8);
    f.write(reinterpret_cast<const char*>(&version), 4);
    f.write(reinterpret_cast<const char*>(&h.w), 4);
    f.write(reinterpret_cast<const char*>(&h.d), 4);
    f.write(reinterpret_cast<const char*>(&h.scale), 4);
    f.write(reinterpret_cast<const char*>(&h.offX), 4);
    f.write(reinterpret_cast<const char*>(&h.offZ), 4);
    f.write(reinterpret_cast<const char*>(h.delta.data()), static_cast<std::streamsize>(h.delta.size() * sizeof(float)));
    if (!f) {
        error = "Could not write " + path.generic_string();
        return false;
    }
    return true;
}

} // namespace eruption
