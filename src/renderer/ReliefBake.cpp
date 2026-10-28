#include "renderer/ReliefBake.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace eruption {

namespace {

int wrap(int i, int n) {
    const int r = i % n;
    return r < 0 ? r + n : r;
}

float edgeLength2(const ReliefVertex& a, const ReliefVertex& b) {
    const float dx = a.pos[0] - b.pos[0], dy = a.pos[1] - b.pos[1], dz = a.pos[2] - b.pos[2];
    return dx * dx + dy * dy + dz * dz;
}

ReliefVertex midpoint(const ReliefVertex& a, const ReliefVertex& b) {
    ReliefVertex m = a;
    for (int k = 0; k < 3; ++k) {
        m.pos[k] = 0.5f * (a.pos[k] + b.pos[k]);
        m.normal[k] = 0.5f * (a.normal[k] + b.normal[k]);
    }
    const float len = std::sqrt(m.normal[0] * m.normal[0] + m.normal[1] * m.normal[1] + m.normal[2] * m.normal[2]);
    if (len > 1e-6f)
        for (float& c : m.normal) c /= len;
    m.uv[0] = 0.5f * (a.uv[0] + b.uv[0]);
    m.uv[1] = 0.5f * (a.uv[1] + b.uv[1]);
    return m;
}

uint64_t edgeKey(uint32_t a, uint32_t b) {
    return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a;
}

// Posicao quantizada a 1/1024 u: a costura de UV tem dois vertices no mesmo
// lugar, e os dois precisam andar o mesmo tanto.
struct PosKey {
    int64_t x, y, z;
    bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct PosHash {
    size_t operator()(const PosKey& k) const {
        return size_t(k.x * 73856093LL) ^ size_t(k.y * 19349663LL) ^ size_t(k.z * 83492791LL);
    }
};
PosKey posKey(const ReliefVertex& v) {
    return {std::llround(double(v.pos[0]) * 1024.0), std::llround(double(v.pos[1]) * 1024.0),
            std::llround(double(v.pos[2]) * 1024.0)};
}

} // namespace

std::vector<uint8_t> reliefMedian(const uint8_t* src, int w, int h, int radius) {
    std::vector<uint8_t> out(src, src + size_t(w) * size_t(h));
    if (radius <= 0 || w <= 0 || h <= 0) return out;
    const int win = 2 * radius + 1;
    const int half = (win * win) / 2; // posicao da mediana na janela ordenada
    // Histograma deslizante por linha (Huang): entra uma coluna, sai outra.
    for (int y = 0; y < h; ++y) {
        int hist[256] = {0};
        for (int dy = -radius; dy <= radius; ++dy)
            for (int dx = -radius; dx <= radius; ++dx) ++hist[src[size_t(wrap(y + dy, h)) * w + size_t(wrap(dx, w))]];
        for (int x = 0; x < w; ++x) {
            if (x > 0) {
                const int xo = wrap(x - radius - 1, w), xi = wrap(x + radius, w);
                for (int dy = -radius; dy <= radius; ++dy) {
                    const size_t row = size_t(wrap(y + dy, h)) * w;
                    --hist[src[row + size_t(xo)]];
                    ++hist[src[row + size_t(xi)]];
                }
            }
            int acc = 0, v = 0;
            while (v < 255 && acc + hist[v] <= half) acc += hist[v++];
            out[size_t(y) * w + size_t(x)] = uint8_t(v);
        }
    }
    return out;
}

float reliefSampleNearest(const std::vector<uint8_t>& map, int w, int h, float u, float v) {
    if (w <= 0 || h <= 0 || map.size() < size_t(w) * size_t(h)) return 0.5f;
    const int x = wrap(int(std::floor(u * float(w))), w);
    const int y = wrap(int(std::floor(v * float(h))), h);
    return float(map[size_t(y) * w + size_t(x)]) / 255.0f;
}

std::vector<uint8_t> reliefBoxBlur(const uint8_t* src, int w, int h, int radius) {
    std::vector<uint8_t> out(src, src + size_t(w) * size_t(h));
    if (radius <= 0 || w <= 0 || h <= 0) return out;
    const int win = 2 * radius + 1;
    std::vector<uint8_t> tmp(out.size());
    // Horizontal: soma deslizante por linha.
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + size_t(y) * w;
        int acc = 0;
        for (int dx = -radius; dx <= radius; ++dx) acc += row[wrap(dx, w)];
        for (int x = 0; x < w; ++x) {
            tmp[size_t(y) * w + x] = uint8_t((acc + win / 2) / win);
            acc += row[wrap(x + radius + 1, w)] - row[wrap(x - radius, w)];
        }
    }
    // Vertical, sobre o resultado horizontal.
    for (int x = 0; x < w; ++x) {
        int acc = 0;
        for (int dy = -radius; dy <= radius; ++dy) acc += tmp[size_t(wrap(dy, h)) * w + x];
        for (int y = 0; y < h; ++y) {
            out[size_t(y) * w + x] = uint8_t((acc + win / 2) / win);
            acc += tmp[size_t(wrap(y + radius + 1, h)) * w + x] - tmp[size_t(wrap(y - radius, h)) * w + x];
        }
    }
    return out;
}

float reliefSampleBilinear(const std::vector<uint8_t>& map, int w, int h, float u, float v) {
    if (w <= 0 || h <= 0 || map.size() < size_t(w) * size_t(h)) return 0.5f;
    const float fx = u * float(w) - 0.5f, fy = v * float(h) - 0.5f;
    const float x0f = std::floor(fx), y0f = std::floor(fy);
    const float tx = fx - x0f, ty = fy - y0f;
    const int x0 = wrap(int(x0f), w), y0 = wrap(int(y0f), h);
    const int x1 = wrap(x0 + 1, w), y1 = wrap(y0 + 1, h);
    auto at = [&](int x, int y) { return float(map[size_t(y) * w + size_t(x)]); };
    const float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const float b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return (a + (b - a) * ty) / 255.0f;
}

ReliefBakeStats bakeRelief(std::vector<ReliefVertex>& verts, std::vector<uint32_t>& indices,
                           const ReliefBakeParams& params, const ReliefHeightSampler& height,
                           ReliefBakeRecord* record) {
    const auto t0 = std::chrono::steady_clock::now();
    ReliefBakeStats st;
    st.trianglesIn = indices.size() / 3;
    const float lim2 = params.spacing * params.spacing;
    std::vector<uint32_t> parents;
    // Triangulo de entrada de cada triangulo atual (a ordem se mantem).
    std::vector<uint32_t> origin(st.trianglesIn);
    for (size_t t = 0; t < origin.size(); ++t) origin[t] = uint32_t(t);

    // Rodadas de partir: toda aresta maior que `spacing` ganha o ponto medio
    // (um so', pela chave da aresta) e cada triangulo vira 2, 3 ou 4 conforme
    // quantas arestas dele partiram.
    for (;;) {
        std::unordered_map<uint64_t, uint32_t> mids;
        const size_t tris = indices.size() / 3;
        size_t grow = 0;
        for (size_t t = 0; t < tris; ++t) {
            int splits = 0;
            for (int e = 0; e < 3; ++e) {
                const uint32_t a = indices[t * 3 + e], b = indices[t * 3 + (e + 1) % 3];
                if (edgeLength2(verts[a], verts[b]) > lim2) {
                    ++splits;
                    mids.emplace(edgeKey(a, b), 0u);
                }
            }
            grow += size_t(splits);
        }
        if (mids.empty()) break;
        if (tris + grow > params.maxTriangles) {
            st.capped = true;
            break;
        }
        // Ordem dos vertices novos pela chave: o resultado nao depende da
        // ordem de iteracao do mapa (cache e entrada iguais = bytes iguais).
        std::vector<uint64_t> keys;
        keys.reserve(mids.size());
        for (const auto& kv : mids) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        for (uint64_t key : keys) {
            const uint32_t a = uint32_t(key >> 32), b = uint32_t(key & 0xffffffffu);
            mids[key] = uint32_t(verts.size());
            verts.push_back(midpoint(verts[a], verts[b]));
            parents.push_back(a);
            parents.push_back(b);
        }
        std::vector<uint32_t> next, nextOrigin;
        next.reserve((tris + grow) * 3);
        nextOrigin.reserve(tris + grow);
        uint32_t from = 0;
        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
            next.push_back(a);
            next.push_back(b);
            next.push_back(c);
            nextOrigin.push_back(from);
        };
        for (size_t t = 0; t < tris; ++t) {
            const uint32_t v[3] = {indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2]};
            from = origin[t];
            uint32_t m[3];
            bool s[3];
            int n = 0;
            for (int e = 0; e < 3; ++e) {
                const auto it = mids.find(edgeKey(v[e], v[(e + 1) % 3]));
                s[e] = it != mids.end();
                m[e] = s[e] ? it->second : 0u;
                n += s[e];
            }
            if (n == 0) {
                tri(v[0], v[1], v[2]);
            } else if (n == 3) {
                tri(v[0], m[0], m[2]);
                tri(m[0], v[1], m[1]);
                tri(m[2], m[1], v[2]);
                tri(m[0], m[1], m[2]);
            } else if (n == 1) {
                const int e = s[0] ? 0 : (s[1] ? 1 : 2); // aresta v[e] -> v[e+1]
                const uint32_t a = v[e], b = v[(e + 1) % 3], c = v[(e + 2) % 3];
                tri(a, m[e], c);
                tri(m[e], b, c);
            } else {
                const int e = !s[0] ? 1 : (!s[1] ? 2 : 0); // partidas: e e e+1; inteira: e+2
                const uint32_t a = v[e], b = v[(e + 1) % 3], c = v[(e + 2) % 3];
                const uint32_t mab = m[e], mbc = m[(e + 1) % 3];
                tri(mab, b, mbc);
                tri(a, mab, mbc);
                tri(a, mbc, c);
            }
        }
        indices.swap(next);
        origin.swap(nextOrigin);
        ++st.rounds;
    }

    // Deslocamento: media por posicao, do tanto e da direcao (a costura de
    // UV e a quina de normal dura andam juntas).
    std::vector<float> d(verts.size(), 0.0f), dir(verts.size() * 3, 0.0f);
    for (size_t i = 0; i < verts.size(); ++i)
        for (int k = 0; k < 3; ++k) dir[i * 3 + k] = verts[i].normal[k];
    if (params.amplitude != 0.0f) {
        struct Acc {
            float d = 0.0f, n[3] = {0.0f, 0.0f, 0.0f};
            int count = 0;
        };
        std::unordered_map<PosKey, Acc, PosHash> byPos;
        byPos.reserve(verts.size());
        for (size_t i = 0; i < verts.size(); ++i) {
            const ReliefVertex& v = verts[i];
            const float hh = height ? height(v.tex, v.uv[0], v.uv[1]) : 0.5f;
            Acc& acc = byPos[posKey(v)];
            acc.d += (hh - 0.5f) * params.amplitude;
            for (int k = 0; k < 3; ++k) acc.n[k] += v.normal[k];
            ++acc.count;
        }
        for (size_t i = 0; i < verts.size(); ++i) {
            const Acc& acc = byPos[posKey(verts[i])];
            d[i] = acc.d / float(acc.count);
            const float len = std::sqrt(acc.n[0] * acc.n[0] + acc.n[1] * acc.n[1] + acc.n[2] * acc.n[2]);
            if (len > 1e-6f)
                for (int k = 0; k < 3; ++k) dir[i * 3 + k] = acc.n[k] / len;
        }
        for (size_t i = 0; i < verts.size(); ++i)
            for (int k = 0; k < 3; ++k) verts[i].pos[k] += dir[i * 3 + k] * d[i];
    }
    if (record) {
        record->parents = std::move(parents);
        record->offset = std::move(d);
        record->dir = std::move(dir);
        record->children.assign(st.trianglesIn, 0u);
        for (uint32_t o : origin) ++record->children[o];
        record->indices = indices;
    }

    st.trianglesOut = indices.size() / 3;
    st.verticesOut = verts.size();
    st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return st;
}

namespace {

struct P {
    float x, y, z;
};

// Altura do topo da malha nos pontos da grade (NaN = sem superficie).
void rasterTop(const ReliefPositions& v, const std::vector<uint32_t>& idx, const ReliefFloorGrid& g,
               std::vector<float>& top) {
    top.assign(size_t(g.nx) * size_t(g.nz), std::numeric_limits<float>::quiet_NaN());
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        if (idx[t] >= v.count || idx[t + 1] >= v.count || idx[t + 2] >= v.count) continue;
        const float *pa = v.at(idx[t]), *pb = v.at(idx[t + 1]), *pc = v.at(idx[t + 2]);
        const P a{pa[0], pa[1], pa[2]}, b{pb[0], pb[1], pb[2]}, c{pc[0], pc[1], pc[2]};
        const float det = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (std::fabs(det) < 1e-9f) continue; // de pe': nao e' chao
        const int x0 = std::max(0, int(std::ceil((std::min({a.x, b.x, c.x}) - g.minX) / g.cell)));
        const int x1 = std::min(g.nx - 1, int(std::floor((std::max({a.x, b.x, c.x}) - g.minX) / g.cell)));
        const int z0 = std::max(0, int(std::ceil((std::min({a.z, b.z, c.z}) - g.minZ) / g.cell)));
        const int z1 = std::min(g.nz - 1, int(std::floor((std::max({a.z, b.z, c.z}) - g.minZ) / g.cell)));
        for (int iz = z0; iz <= z1; ++iz)
            for (int ix = x0; ix <= x1; ++ix) {
                const float px = g.minX + float(ix) * g.cell, pz = g.minZ + float(iz) * g.cell;
                const float l1 = ((b.z - c.z) * (px - c.x) + (c.x - b.x) * (pz - c.z)) / det;
                const float l2 = ((c.z - a.z) * (px - c.x) + (a.x - c.x) * (pz - c.z)) / det;
                const float l3 = 1.0f - l1 - l2;
                constexpr float kE = -1e-4f;
                if (l1 < kE || l2 < kE || l3 < kE) continue;
                const float y = l1 * a.y + l2 * b.y + l3 * c.y;
                float& o = top[size_t(iz) * size_t(g.nx) + size_t(ix)];
                if (!(o >= y)) o = y; // NaN ou mais baixo
            }
    }
}

} // namespace

bool ReliefFloorGrid::sample(float x, float z, float& delta, float& baseY) const {
    if (dy.empty()) return false;
    const float fx = (x - minX) / cell, fz = (z - minZ) / cell;
    if (fx < 0.0f || fz < 0.0f || fx > float(nx - 1) || fz > float(nz - 1)) return false;
    const int ix = std::min(int(fx), nx - 2), iz = std::min(int(fz), nz - 2);
    const float tx = fx - float(ix), tz = fz - float(iz);
    const size_t i0 = size_t(iz) * size_t(nx) + size_t(ix), i1 = i0 + size_t(nx);
    const int16_t b[4] = {base[i0], base[i0 + 1], base[i1], base[i1 + 1]};
    for (int16_t v : b)
        if (v == kNone) return false; // beira da malha: o chao de antes vale
    auto lerp = [&](const int16_t* v) {
        return (float(v[0]) * (1.0f - tx) + float(v[1]) * tx) * (1.0f - tz) +
               (float(v[2]) * (1.0f - tx) + float(v[3]) * tx) * tz;
    };
    const int16_t d[4] = {dy[i0], dy[i0 + 1], dy[i1], dy[i1 + 1]};
    delta = lerp(d) * dyStep;
    baseY = baseMin + lerp(b) * baseStep;
    return true;
}

ReliefFloorGrid reliefFloorGrid(const ReliefPositions& before, const std::vector<uint32_t>& idxBefore,
                                const ReliefPositions& after, const std::vector<uint32_t>& idxAfter, float cell) {
    ReliefFloorGrid g;
    if (before.count == 0 || cell <= 0.0f) return g;
    float mnx = before.at(0)[0], mxx = mnx, mnz = before.at(0)[2], mxz = mnz;
    for (uint32_t i = 0; i < before.count; ++i) {
        const float* q = before.at(i);
        mnx = std::min(mnx, q[0]);
        mxx = std::max(mxx, q[0]);
        mnz = std::min(mnz, q[2]);
        mxz = std::max(mxz, q[2]);
    }
    g.cell = cell;
    g.minX = mnx;
    g.minZ = mnz;
    g.nx = int(std::ceil((mxx - mnx) / cell)) + 2;
    g.nz = int(std::ceil((mxz - mnz) / cell)) + 2;
    if (g.nx < 2 || g.nz < 2 || size_t(g.nx) * size_t(g.nz) > (size_t(1) << 24)) return {};
    std::vector<float> hb, ha;
    rasterTop(before, idxBefore, g, hb);
    rasterTop(after, idxAfter, g, ha);
    float dMax = 0.0f, bMin = std::numeric_limits<float>::max(), bMax = -bMin;
    for (size_t i = 0; i < hb.size(); ++i) {
        if (hb[i] != hb[i]) continue;
        if (ha[i] == ha[i]) dMax = std::max(dMax, std::fabs(ha[i] - hb[i]));
        bMin = std::min(bMin, hb[i]);
        bMax = std::max(bMax, hb[i]);
    }
    // Nada subiu nem desceu um centesimo: o chao de antes vale (sem grade).
    if (bMin > bMax || dMax < 0.01f) return {};
    g.dyStep = std::max(dMax, 1e-3f) / 32767.0f;
    g.baseMin = bMin;
    g.baseStep = std::max(bMax - bMin, 1e-3f) / 32766.0f;
    g.dy.assign(hb.size(), 0);
    g.base.assign(hb.size(), ReliefFloorGrid::kNone);
    for (size_t i = 0; i < hb.size(); ++i) {
        if (hb[i] != hb[i]) continue;
        g.base[i] = int16_t(std::lround((hb[i] - bMin) / g.baseStep));
        if (ha[i] == ha[i]) g.dy[i] = int16_t(std::lround((ha[i] - hb[i]) / g.dyStep));
    }
    return g;
}

} // namespace eruption
