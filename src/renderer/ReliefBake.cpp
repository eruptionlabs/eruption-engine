#include "renderer/ReliefBake.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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

} // namespace eruption
