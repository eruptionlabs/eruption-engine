// Colisao da camera (camera.collide): a orbita encurta quando o relevo fica
// entre o alvo e a lente. Dois testes: a grade de altura (.ter) do mapa
// aberto e dos vizinhos, que cobre o chao por cima, e um raio contra as
// malhas de chao, que ve' o que a grade nao ve': o lado de baixo de um
// morro sobre uma caverna. Sem o segundo, a lente entrava na rocha do teto
// e a tela ficava 90% preta com a camera a 50 graus.
#include "core/Engine.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace eruption {

// Triangulos de uma malha de chao em grade uniforme (espaco do modelo): o
// raio so' testa as celulas que cruza. O chao de um mapa tem ~250 mil
// triangulos; testar todos por quadro seria ~2 ms.
struct GroundRayCache {
    struct Grid {
        const void* data = nullptr; // vertices do no' (valida o cache)
        size_t vertexCount = 0, indexCount = 0;
        Vec3 lo{0.0f};
        float cell = 1.0f;
        glm::ivec3 dims{1};
        std::vector<uint32_t> start; // CSR: celula c usa tris[start[c]..start[c+1])
        std::vector<uint32_t> tris;  // primeiro indice do triangulo em `indices`
    };
    std::unordered_map<std::string, Grid> grids;
    const void* map = nullptr; // mapa aberto quando as grades foram montadas
};

namespace {

constexpr int kMaxCells = 64; // celulas no eixo mais comprido

void buildGrid(GroundRayCache::Grid& g, const ModelNode& node) {
    const auto& vs = node.vertices;
    const auto& ix = node.indices;
    g.data = vs.data();
    g.vertexCount = vs.size();
    g.indexCount = ix.size();
    Vec3 lo(FLT_MAX), hi(-FLT_MAX);
    for (const auto& v : vs) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    const Vec3 ext = glm::max(hi - lo, Vec3(1e-3f));
    g.cell = std::max(std::max({ext.x, ext.y, ext.z}) / kMaxCells, 1.0f);
    g.lo = lo;
    g.dims = glm::clamp(glm::ivec3(glm::ceil(ext / g.cell)), glm::ivec3(1), glm::ivec3(kMaxCells));
    const size_t cells = size_t(g.dims.x) * g.dims.y * g.dims.z;
    auto cellRange = [&](size_t t, glm::ivec3& c0, glm::ivec3& c1) {
        const Vec3 a = vs[ix[t]].position, b = vs[ix[t + 1]].position, c = vs[ix[t + 2]].position;
        c0 = glm::clamp(glm::ivec3(glm::floor((glm::min(glm::min(a, b), c) - g.lo) / g.cell)), glm::ivec3(0), g.dims - 1);
        c1 = glm::clamp(glm::ivec3(glm::floor((glm::max(glm::max(a, b), c) - g.lo) / g.cell)), glm::ivec3(0), g.dims - 1);
    };
    auto cellIndex = [&](int x, int y, int z) { return (size_t(z) * g.dims.y + y) * g.dims.x + x; };
    std::vector<uint32_t> count(cells + 1, 0);
    glm::ivec3 c0, c1;
    for (size_t t = 0; t + 2 < ix.size(); t += 3) {
        if (ix[t] >= vs.size() || ix[t + 1] >= vs.size() || ix[t + 2] >= vs.size()) continue;
        cellRange(t, c0, c1);
        for (int z = c0.z; z <= c1.z; ++z)
            for (int y = c0.y; y <= c1.y; ++y)
                for (int x = c0.x; x <= c1.x; ++x) ++count[cellIndex(x, y, z) + 1];
    }
    for (size_t c = 0; c < cells; ++c) count[c + 1] += count[c];
    g.start = count;
    g.tris.assign(count[cells], 0);
    for (size_t t = 0; t + 2 < ix.size(); t += 3) {
        if (ix[t] >= vs.size() || ix[t + 1] >= vs.size() || ix[t + 2] >= vs.size()) continue;
        cellRange(t, c0, c1);
        for (int z = c0.z; z <= c1.z; ++z)
            for (int y = c0.y; y <= c1.y; ++y)
                for (int x = c0.x; x <= c1.x; ++x) g.tris[count[cellIndex(x, y, z)]++] = uint32_t(t);
    }
}

// Moller-Trumbore dos dois lados; `front` = o raio entra pela face da frente
// (det = -n.d > 0; `side` -1 inverte para instancia espelhada). t no
// parametro de `d` (nao normalizado).
bool hitTriangle(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c, float side, float& t,
                 bool& front) {
    const Vec3 e1 = b - a, e2 = c - a, pv = glm::cross(d, e2);
    const float det = glm::dot(e1, pv);
    if (std::abs(det) < 1e-12f) return false;
    front = det * side > 0.0f;
    const float inv = 1.0f / det;
    const Vec3 tv = o - a;
    const float u = glm::dot(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 qv = glm::cross(tv, e1);
    const float v = glm::dot(d, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = glm::dot(e2, qv) * inv;
    return t > 0.0f;
}

// Entrada e saida do raio numa caixa (slabs); falso se nao cruza em [0, maxT].
bool clipBox(const Vec3& o, const Vec3& d, const Vec3& lo, const Vec3& hi, float maxT, float& t0, float& t1) {
    t0 = 0.0f;
    t1 = maxT;
    for (int k = 0; k < 3; ++k) {
        if (std::abs(d[k]) < 1e-12f) {
            if (o[k] < lo[k] || o[k] > hi[k]) return false;
            continue;
        }
        float a = (lo[k] - o[k]) / d[k], b = (hi[k] - o[k]) / d[k];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    return true;
}

// Todos os cruzamentos da malha no raio ate' maxT (espaco do modelo),
// percorrendo as celulas em ordem (DDA 3D). Um triangulo em varias celulas
// sai repetido; quem chama ordena e tira os repetidos.
void traceGrid(const GroundRayCache::Grid& g, const ModelNode& node, const Vec3& o, const Vec3& d, float maxT, float side,
               std::vector<std::pair<float, bool>>& hits) {
    const Vec3 hi = g.lo + Vec3(g.dims) * g.cell;
    float t0, t1;
    if (!clipBox(o, d, g.lo, hi, maxT, t0, t1)) return;
    const Vec3 p = o + d * t0;
    glm::ivec3 c = glm::clamp(glm::ivec3(glm::floor((p - g.lo) / g.cell)), glm::ivec3(0), g.dims - 1);
    glm::ivec3 step;
    Vec3 tMax, tDelta;
    for (int k = 0; k < 3; ++k) {
        if (std::abs(d[k]) < 1e-12f) {
            step[k] = 0;
            tMax[k] = FLT_MAX;
            tDelta[k] = FLT_MAX;
            continue;
        }
        step[k] = d[k] > 0.0f ? 1 : -1;
        const float edge = g.lo[k] + (c[k] + (d[k] > 0.0f ? 1 : 0)) * g.cell;
        tMax[k] = (edge - o[k]) / d[k];
        tDelta[k] = g.cell / std::abs(d[k]);
    }
    const auto& vs = node.vertices;
    const auto& ix = node.indices;
    for (;;) {
        const size_t ci = (size_t(c.z) * g.dims.y + c.y) * g.dims.x + c.x;
        for (uint32_t k = g.start[ci]; k < g.start[ci + 1]; ++k) {
            const uint32_t t = g.tris[k];
            float h;
            bool front;
            if (hitTriangle(o, d, vs[ix[t]].position, vs[ix[t + 1]].position, vs[ix[t + 2]].position, side, h, front) &&
                h <= maxT)
                hits.emplace_back(h, front);
        }
        const float exitT = std::min({tMax.x, tMax.y, tMax.z});
        if (exitT > t1) break;
        const int axis = tMax.x <= tMax.y && tMax.x <= tMax.z ? 0 : (tMax.y <= tMax.z ? 1 : 2);
        c[axis] += step[axis];
        if (c[axis] < 0 || c[axis] >= g.dims[axis]) break;
        tMax[axis] += tDelta[axis];
    }
}

// No' de relevo da instancia (chao sem balanco, com triangulos) ou nulo.
const ModelNode* groundRayNode(const ModelRenderer& mr, const ModelInstance& in) {
    if (!in.enabled || !mr.isGroundMesh(in.meshIndex)) return nullptr;
    // Capim e' chao com balanco: nao e' relevo.
    const ModelMeshGPU* mesh = mr.meshAt(in.meshIndex);
    if (!mesh || mesh->swayAmount > 0.0f) return nullptr;
    const ModelAsset* asset = mr.asset(in.assetPath);
    if (!asset || in.nodeIndex >= asset->nodes.size()) return nullptr;
    const ModelNode& node = asset->nodes[in.nodeIndex];
    return node.indices.size() < 3 || node.vertices.empty() ? nullptr : &node;
}

bool gridMatches(const GroundRayCache::Grid& g, const ModelNode& node) {
    return g.data == node.vertices.data() && g.vertexCount == node.vertices.size() && g.indexCount == node.indices.size();
}

} // namespace

// Monta as grades do relevo em cena e solta as de quem saiu. Chamado no
// quadro que ja' encaixa o vizinho (Engine::finishRegion), ao ligar a
// colisao e no primeiro quadro de um mapa novo; o raio nunca monta: o
// vizinho entra em varios quadros e montar na primeira consulta dava 12 a
// 18 ms num quadro comum (morro de 255 mil triangulos). Sem grade, a malha
// fica de fora ate' o encaixe e vale a grade de altura.
void Engine::warmCameraCollision() {
    if (!m_cameraCollide) return;
    if (!m_groundRays) m_groundRays = std::make_shared<GroundRayCache>();
    std::unordered_set<std::string> live;
    for (const ModelInstance& in : m_modelRenderer.getInstances())
        if (const ModelNode* node = groundRayNode(m_modelRenderer, in)) {
            const std::string key = in.assetPath + '#' + std::to_string(in.nodeIndex);
            live.insert(key);
            GroundRayCache::Grid& g = m_groundRays->grids[key];
            if (!gridMatches(g, *node)) buildGrid(g, *node);
        }
    auto& grids = m_groundRays->grids;
    for (auto it = grids.begin(); it != grids.end();) it = live.count(it->first) ? std::next(it) : grids.erase(it);
    m_groundRays->map = m_currentMap.get();
}

void Engine::raycastGroundMeshes(const Vec3& origin, const Vec3& dir, float maxT,
                                 std::vector<std::pair<float, bool>>& hits) {
    hits.clear();
    if (!m_groundRays) m_groundRays = std::make_shared<GroundRayCache>();
    const Vec3 end = origin + dir * maxT;
    const Vec3 segLo = glm::min(origin, end), segHi = glm::max(origin, end);
    for (const ModelInstance& in : m_modelRenderer.getInstances()) {
        if (glm::any(glm::lessThan(in.worldAabbMax, segLo)) || glm::any(glm::greaterThan(in.worldAabbMin, segHi))) continue;
        float t0, t1;
        if (!clipBox(origin, dir, in.worldAabbMin, in.worldAabbMax, maxT, t0, t1)) continue;
        const ModelNode* node = groundRayNode(m_modelRenderer, in);
        if (!node) continue;
        const auto it = m_groundRays->grids.find(in.assetPath + '#' + std::to_string(in.nodeIndex));
        if (it == m_groundRays->grids.end() || !gridMatches(it->second, *node)) continue;
        // Raio no espaco do modelo; t continua valendo no mundo porque a
        // direcao local nao e' normalizada.
        const Mat4 inv = glm::inverse(in.transform);
        const float side = glm::determinant(Mat3(in.transform)) < 0.0f ? -1.0f : 1.0f;
        traceGrid(it->second, *node, Vec3(inv * Vec4(origin, 1.0f)), Vec3(inv * Vec4(dir, 0.0f)), maxT, side, hits);
    }
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end(),
                           [](const auto& x, const auto& y) { return x.second == y.second && y.first - x.first < 1e-4f; }),
               hits.end());
}

void Engine::setCameraCollide(bool on, float margin) {
    m_cameraCollide = on;
    m_cameraCollideMargin = std::max(0.0f, margin);
    if (on) warmCameraCollision();
    else m_groundRays.reset();
}

// Braco de mola. Duas regras:
// - a lente nunca fica DENTRO do relevo (tela preta): o raio do alvo a' lente
//   conta entradas (face da frente) e saidas (de tras) das malhas de chao, e
//   a grade de altura diz se a lente esta' abaixo do chao; dentro, a lente
//   para antes da ultima entrada;
// - o que so' TAPA (morro entre o alvo e a lente) aproxima a camera quando
//   ela fica a pelo menos kOcclusionShare da distancia pedida. Mais perto que
//   isso a camera grudava no grupo atras de cada pedra (arco a 8 u do alvo) e
//   quem cuida e' o camera.see_through.
// Entra rapido e volta devagar (sem bombear num morro que vai e vem). So' o
// relevo: modelo fino (arvore, poste) nao conta.
void Engine::updateCameraCollision(float dt) {
    if (!m_cameraCollide || m_camera.isOrthographic() || !m_currentMap) {
        m_cameraCollideDist = -1.0f;
        m_camera.setOrbitClamp(FLT_MAX);
        return;
    }
    if (!m_groundRays || m_groundRays->map != m_currentMap.get()) warmCameraCollision();
    const float want = m_camera.orbitDistance();
    const float yaw = m_camera.orbitYaw(), pitch = m_camera.orbitPitch();
    const Vec3 dir(std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw));
    const Vec3 target = m_camera.target();
    const float margin = m_cameraCollideMargin;
    constexpr float kMinDist = 8.0f, kOcclusionShare = 0.35f;
    const float reach = want + margin;

    static thread_local std::vector<std::pair<float, bool>> hits;
    raycastGroundMeshes(target, dir, reach, hits);
    // Dentro = o ultimo cruzamento antes da lente e' uma ENTRADA (face da
    // frente); para antes dela. Contar entradas e saidas (paridade) errava
    // com malha aberta: duas superficies coincidentes a 88 u viravam
    // "+88 -88" ou "-88 +88" conforme o empate e a lente ficava "fora"
    // dentro da pedra, ou o contrario.
    float occl = FLT_MAX, inside = FLT_MAX;
    const std::pair<float, bool>* last = nullptr;
    for (const auto& h : hits) {
        if (h.second) occl = std::min(occl, h.first);
        if (h.first <= want) last = &h;
    }
    if (last && last->second) inside = last->first;
    // Grade de altura: so' com o alvo acima dela (dentro de uma caverna a
    // grade e' o topo do morro, nao o teto que a lente encontra) e com um
    // mapa sob ele (fora de todos a altura vale 0 e o raio inteiro parecia
    // "debaixo do chao").
    const float targetClear = target.y - sampleTerrainHeight(target.x, target.z);
    if (targetClear >= 0.0f && mapAt(target.x, target.z)) {
        float gridT = 0.0f;
        if (raycastTerrain(target, dir, reach, gridT)) occl = std::min(occl, gridT);
        // Lente abaixo do chao + folga: volta ate' o ultimo ponto com folga.
        // A folga nao passa da que o proprio alvo tem (alvo no pe' do grupo,
        // a 2 u do chao): senao o raio ja' nascia "sem folga" e a camera ia
        // parar na cabeca.
        const float hfMargin = std::min(margin, 0.5f * targetClear);
        auto clear = [&](float t) {
            const Vec3 p = target + dir * t;
            return p.y - sampleTerrainHeight(p.x, p.z) - hfMargin;
        };
        if (clear(want) < 0.0f) {
            constexpr float kStep = 4.0f;
            float a = want;
            while (a > 0.0f && clear(a) < 0.0f) a -= kStep;
            a = std::max(a, 0.0f);
            float b = std::min(a + kStep, want);
            for (int i = 0; i < 6; ++i) {
                const float m = 0.5f * (a + b);
                (clear(m) < 0.0f ? b : a) = m;
            }
            inside = std::min(inside, a + 0.5f); // `a` ja' tem a folga: o limite duro cai nele
        }
    }
    float stop = inside;
    if (occl >= kOcclusionShare * want) stop = std::min(stop, occl);
    float limit = want, hard = want;
    if (stop < FLT_MAX) {
        // Parede a menos de kMinDist do alvo: a lente chega perto da cabeca,
        // mas nao entra na pedra.
        hard = std::max(0.5f, stop - 0.5f);
        limit = std::max(std::min(kMinDist, hard), stop - margin);
    }
    limit = std::min(limit, hard);
    float cur = m_cameraCollideDist < 0.0f ? want : m_cameraCollideDist;
    // Entra em ~0,06 s e sai em ~0,35 s (constantes de tempo).
    const float tau = limit < cur ? 0.06f : 0.35f;
    cur += (limit - cur) * (1.0f - std::exp(-std::max(dt, 0.0f) / tau));
    cur = std::min({cur, hard, want});
    m_cameraCollideDist = cur;
    m_camera.setOrbitClamp(cur >= want - 1e-3f ? FLT_MAX : cur);
}

} // namespace eruption
