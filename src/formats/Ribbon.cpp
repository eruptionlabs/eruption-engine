#include "formats/Ribbon.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

// Sem FMA: a*b+c contraido em uma instrucao arredonda uma vez so' e muda os
// bits entre o PC (x86, sem FMA por padrao) e o ARM (que contrai). Ver o
// cabecalho: o hash da faixa tem de ser o mesmo nas duas.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace eruption {

namespace {

// Subdivisoes por trecho de controle para medir o comprimento de arco: 64
// deixa o passo final a menos de 0,1% do pedido numa curva fechada de raio
// igual a 2 trechos (medido no teste).
constexpr int kDense = 64;

struct P2 {
    float x, z;
};

P2 lerp2(const P2& a, const P2& b, float ta, float tb, float t) {
    const float d = tb - ta;
    const float wa = (tb - t) / d, wb = (t - ta) / d;
    return {wa * a.x + wb * b.x, wa * a.z + wb * b.z};
}

// Intervalo de no' centripeto: |p1 - p0|^0,5 = raiz da raiz do quadrado.
float knot(const P2& a, const P2& b) {
    const float dx = b.x - a.x, dz = b.z - a.z;
    return std::sqrt(std::sqrt(dx * dx + dz * dz));
}

// Catmull-Rom centripeta entre p1 e p2 (Barry-Goldman), u em [0, 1].
P2 catmullRom(const P2& p0, const P2& p1, const P2& p2, const P2& p3, float u) {
    const float t0 = 0.0f;
    const float t1 = t0 + knot(p0, p1);
    const float t2 = t1 + knot(p1, p2);
    const float t3 = t2 + knot(p2, p3);
    const float t = t1 + (t2 - t1) * u;
    const P2 a1 = lerp2(p0, p1, t0, t1, t);
    const P2 a2 = lerp2(p1, p2, t1, t2, t);
    const P2 a3 = lerp2(p2, p3, t2, t3, t);
    const P2 b1 = lerp2(a1, a2, t0, t2, t);
    const P2 b2 = lerp2(a2, a3, t1, t3, t);
    return lerp2(b1, b2, t1, t2, t);
}

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void add(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
};

} // namespace

int Ribbon::segmentCount() const {
    const int n = static_cast<int>(m_samples.size());
    return m_closed ? n : n - 1;
}

bool Ribbon::build(const std::vector<float>& pointsXZ, const std::vector<float>& widths, const RibbonParams& params,
                   std::string& error) {
    m_samples.clear();
    m_heights.clear();
    m_cols = 0;
    m_length = 0.0f;
    m_closed = params.closed;
    m_step = params.step > 0.01f ? params.step : 0.01f;
    m_uvLength = params.uvLength > 0.0f ? params.uvLength : 1.0f;

    // Pontos repetidos em seguida dao no' de intervalo zero: saem.
    std::vector<P2> pts;
    std::vector<float> ws;
    const size_t count = pointsXZ.size() / 2;
    if (!widths.empty() && widths.size() != 1 && widths.size() != count) {
        error = "widths: one value, or one per point";
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const P2 p{pointsXZ[2 * i], pointsXZ[2 * i + 1]};
        const float w = widths.empty() ? 1.0f : widths[widths.size() == 1 ? 0 : i];
        if (!std::isfinite(p.x) || !std::isfinite(p.z) || !std::isfinite(w) || w <= 0.0f) {
            error = "point " + std::to_string(i + 1) + ": not a finite position with positive width";
            return false;
        }
        if (!pts.empty() && pts.back().x == p.x && pts.back().z == p.z) continue;
        pts.push_back(p);
        ws.push_back(w);
    }
    if (m_closed && pts.size() > 1 && pts.front().x == pts.back().x && pts.front().z == pts.back().z) {
        pts.pop_back();
        ws.pop_back();
    }
    const size_t n = pts.size();
    if (n < 2 || (m_closed && n < 3)) {
        error = m_closed ? "a closed ribbon needs 3 distinct points" : "a ribbon needs 2 distinct points";
        return false;
    }

    // Linha densa: kDense pontos por trecho, com o comprimento acumulado.
    auto ctrl = [&](long i) -> P2 {
        const long m = static_cast<long>(n);
        if (m_closed) return pts[static_cast<size_t>(((i % m) + m) % m)];
        if (i < 0) return {2.0f * pts[0].x - pts[1].x, 2.0f * pts[0].z - pts[1].z};
        if (i >= m) return {2.0f * pts[n - 1].x - pts[n - 2].x, 2.0f * pts[n - 1].z - pts[n - 2].z};
        return pts[static_cast<size_t>(i)];
    };
    const size_t spans = m_closed ? n : n - 1;
    std::vector<P2> dense;
    std::vector<float> denseW, denseS;
    dense.reserve(spans * kDense + 1);
    for (size_t k = 0; k < spans; ++k) {
        const long i = static_cast<long>(k);
        const P2 p0 = ctrl(i - 1), p1 = ctrl(i), p2 = ctrl(i + 1), p3 = ctrl(i + 2);
        const float w1 = ws[k], w2 = ws[(k + 1) % n];
        for (int j = 0; j < kDense; ++j) {
            const float u = static_cast<float>(j) / static_cast<float>(kDense);
            dense.push_back(j == 0 ? p1 : catmullRom(p0, p1, p2, p3, u));
            denseW.push_back(w1 + (w2 - w1) * u);
        }
    }
    dense.push_back(m_closed ? pts[0] : pts[n - 1]);
    denseW.push_back(m_closed ? ws[0] : ws[n - 1]);
    denseS.resize(dense.size());
    denseS[0] = 0.0f;
    for (size_t i = 1; i < dense.size(); ++i) {
        const float dx = dense[i].x - dense[i - 1].x, dz = dense[i].z - dense[i - 1].z;
        denseS[i] = denseS[i - 1] + std::sqrt(dx * dx + dz * dz);
    }
    m_length = denseS.back();
    if (!(m_length > 0.0f)) {
        error = "ribbon has zero length";
        return false;
    }

    // Reamostra a passo constante de arco. Fechada: sem repetir o comeco no fim.
    int intervals = static_cast<int>(m_length / m_step + 0.5f);
    if (intervals < (m_closed ? 3 : 1)) intervals = m_closed ? 3 : 1;
    const int sampleCount = m_closed ? intervals : intervals + 1;
    const float spacing = m_length / static_cast<float>(intervals);
    m_samples.resize(static_cast<size_t>(sampleCount));
    size_t d = 0;
    for (int i = 0; i < sampleCount; ++i) {
        const float s = i == intervals ? m_length : spacing * static_cast<float>(i);
        while (d + 2 < dense.size() && denseS[d + 1] < s) ++d;
        const float len = denseS[d + 1] - denseS[d];
        const float f = len > 0.0f ? std::min(1.0f, std::max(0.0f, (s - denseS[d]) / len)) : 0.0f;
        RibbonSample& r = m_samples[static_cast<size_t>(i)];
        r.x = dense[d].x + (dense[d + 1].x - dense[d].x) * f;
        r.z = dense[d].z + (dense[d + 1].z - dense[d].z) * f;
        r.width = denseW[d] + (denseW[d + 1] - denseW[d]) * f;
        r.s = s;
    }
    // Tangente pela diferenca central (nas pontas da aberta, de um lado so').
    for (int i = 0; i < sampleCount; ++i) {
        int a = i - 1, b = i + 1;
        if (m_closed) {
            a = (a + sampleCount) % sampleCount;
            b %= sampleCount;
        } else {
            a = std::max(a, 0);
            b = std::min(b, sampleCount - 1);
        }
        const float dx = m_samples[static_cast<size_t>(b)].x - m_samples[static_cast<size_t>(a)].x;
        const float dz = m_samples[static_cast<size_t>(b)].z - m_samples[static_cast<size_t>(a)].z;
        const float len = std::sqrt(dx * dx + dz * dz);
        RibbonSample& r = m_samples[static_cast<size_t>(i)];
        if (len > 0.0f) {
            r.tx = dx / len;
            r.tz = dz / len;
        }
    }
    buildGrid();
    return true;
}

void Ribbon::buildGrid() {
    float x0 = m_samples[0].x, x1 = x0, z0 = m_samples[0].z, z1 = z0, wMax = 0.0f, wMin = m_samples[0].width;
    for (const RibbonSample& r : m_samples) {
        wMin = std::min(wMin, r.width);
        x0 = std::min(x0, r.x);
        x1 = std::max(x1, r.x);
        z0 = std::min(z0, r.z);
        z1 = std::max(z1, r.z);
        wMax = std::max(wMax, r.width);
    }
    // Celula da ordem da largura: um ponto da faixa cai numa celula que lista
    // poucos segmentos. A margem e' a meia-largura mais um passo.
    const float margin = wMax * 0.5f + m_step;
    m_minMargin = wMin * 0.5f + m_step;
    m_cell = std::max(wMax, 4.0f * m_step);
    m_gridX0 = x0 - margin;
    m_gridZ0 = z0 - margin;
    m_gridW = static_cast<int>((x1 + margin - m_gridX0) / m_cell) + 1;
    m_gridD = static_cast<int>((z1 + margin - m_gridZ0) / m_cell) + 1;
    const int segs = segmentCount();
    const size_t n = m_samples.size();
    auto cellRange = [&](int seg, int& cx0, int& cx1, int& cz0, int& cz1) {
        const RibbonSample& a = m_samples[static_cast<size_t>(seg)];
        const RibbonSample& b = m_samples[(static_cast<size_t>(seg) + 1) % n];
        const float m = std::max(a.width, b.width) * 0.5f + m_step;
        cx0 = std::max(0, static_cast<int>((std::min(a.x, b.x) - m - m_gridX0) / m_cell));
        cx1 = std::min(m_gridW - 1, static_cast<int>((std::max(a.x, b.x) + m - m_gridX0) / m_cell));
        cz0 = std::max(0, static_cast<int>((std::min(a.z, b.z) - m - m_gridZ0) / m_cell));
        cz1 = std::min(m_gridD - 1, static_cast<int>((std::max(a.z, b.z) + m - m_gridZ0) / m_cell));
    };
    // Duas passadas (conta, depois preenche): lista compacta por celula.
    m_cellStart.assign(static_cast<size_t>(m_gridW) * static_cast<size_t>(m_gridD) + 1, 0);
    int cx0, cx1, cz0, cz1;
    for (int s = 0; s < segs; ++s) {
        cellRange(s, cx0, cx1, cz0, cz1);
        for (int z = cz0; z <= cz1; ++z)
            for (int x = cx0; x <= cx1; ++x) ++m_cellStart[static_cast<size_t>(z * m_gridW + x) + 1];
    }
    for (size_t i = 1; i < m_cellStart.size(); ++i) m_cellStart[i] += m_cellStart[i - 1];
    m_cellSegs.assign(m_cellStart.back(), 0);
    std::vector<uint32_t> fill(m_cellStart.begin(), m_cellStart.end() - 1);
    for (int s = 0; s < segs; ++s) {
        cellRange(s, cx0, cx1, cz0, cz1);
        for (int z = cz0; z <= cz1; ++z)
            for (int x = cx0; x <= cx1; ++x) m_cellSegs[fill[static_cast<size_t>(z * m_gridW + x)]++] = static_cast<uint32_t>(s);
    }
}

void Ribbon::segmentDistance(int seg, float x, float z, float& d2, float& u) const {
    const RibbonSample& a = m_samples[static_cast<size_t>(seg)];
    const RibbonSample& b = m_samples[(static_cast<size_t>(seg) + 1) % m_samples.size()];
    const float ex = b.x - a.x, ez = b.z - a.z;
    const float len2 = ex * ex + ez * ez;
    u = len2 > 0.0f ? ((x - a.x) * ex + (z - a.z) * ez) / len2 : 0.0f;
    u = std::min(1.0f, std::max(0.0f, u));
    const float dx = x - (a.x + ex * u), dz = z - (a.z + ez * u);
    d2 = dx * dx + dz * dz;
}

RibbonQuery Ribbon::query(float x, float z) const {
    RibbonQuery q;
    const int segs = segmentCount();
    if (segs <= 0) return q;
    int best = -1;
    float bestD2 = 0.0f, bestU = 0.0f;
    auto consider = [&](int s) {
        float d2, u;
        segmentDistance(s, x, z, d2, u);
        // Empate: o menor indice, para a resposta nao depender da ordem da busca.
        if (best < 0 || d2 < bestD2 || (d2 == bestD2 && s < best)) {
            best = s;
            bestD2 = d2;
            bestU = u;
        }
    };
    // A celula do ponto lista todo segmento a menos da propria margem dele (a
    // meia-largura mais um passo). Se o melhor dela esta' a menos da menor
    // margem, nenhum segmento fora da lista pode estar mais perto: o resultado
    // e' o mais proximo de todos, exato. Senao (ponto longe da faixa), todos.
    const int gx = static_cast<int>(std::floor((x - m_gridX0) / m_cell));
    const int gz = static_cast<int>(std::floor((z - m_gridZ0) / m_cell));
    if (gx >= 0 && gz >= 0 && gx < m_gridW && gz < m_gridD) {
        const size_t c = static_cast<size_t>(gz * m_gridW + gx);
        for (uint32_t i = m_cellStart[c]; i < m_cellStart[c + 1]; ++i) consider(static_cast<int>(m_cellSegs[i]));
    }
    if (best < 0 || bestD2 > m_minMargin * m_minMargin)
        for (int s = 0; s < segs; ++s) consider(s);

    const size_t n = m_samples.size();
    const RibbonSample& a = m_samples[static_cast<size_t>(best)];
    const size_t bi = (static_cast<size_t>(best) + 1) % n;
    const RibbonSample& b = m_samples[bi];
    const float cx = a.x + (b.x - a.x) * bestU, cz = a.z + (b.z - a.z) * bestU;
    float tx = a.tx + (b.tx - a.tx) * bestU, tz = a.tz + (b.tz - a.tz) * bestU;
    const float tl = std::sqrt(tx * tx + tz * tz);
    if (tl > 0.0f) {
        tx /= tl;
        tz /= tl;
    }
    const float rx = tz, rz = -tx; // direita
    const float bs = bi == 0 ? m_length : b.s; // fechada: o ultimo segmento volta ao comeco
    q.ok = true;
    q.segment = best;
    q.s = a.s + (bs - a.s) * bestU;
    q.t = m_length > 0.0f ? q.s / m_length : 0.0f;
    q.width = a.width + (b.width - a.width) * bestU;
    q.lateral = (x - cx) * rx + (z - cz) * rz;
    q.edge = q.width * 0.5f - std::abs(q.lateral);
    q.nx = q.lateral >= 0.0f ? -rx : rx;
    q.nz = q.lateral >= 0.0f ? -rz : rz;
    q.tx = tx;
    q.tz = tz;
    q.x = cx;
    q.z = cz;
    q.height = heightAt(static_cast<size_t>(best), bi, bestU, q.lateral / q.width + 0.5f);
    return q;
}

float Ribbon::heightAt(size_t a, size_t b, float u, float lateralFrac) const {
    if (m_cols < 2) return 0.0f;
    // Bilinear: entre as duas amostras e entre as duas colunas vizinhas.
    const float f = std::min(1.0f, std::max(0.0f, lateralFrac)) * static_cast<float>(m_cols - 1);
    const int c0 = std::min(static_cast<int>(f), m_cols - 2);
    const float fc = f - static_cast<float>(c0);
    auto h = [&](size_t s, int c) { return m_heights[s * static_cast<size_t>(m_cols) + static_cast<size_t>(c)]; };
    const float ha = h(a, c0) + (h(a, c0 + 1) - h(a, c0)) * fc;
    const float hb = h(b, c0) + (h(b, c0 + 1) - h(b, c0)) * fc;
    return ha + (hb - ha) * u;
}

RibbonQuery Ribbon::point(float t) const {
    RibbonQuery q;
    const size_t n = m_samples.size();
    if (n < 2 || !std::isfinite(t)) return q;
    if (m_closed) t -= std::floor(t);
    else t = std::min(1.0f, std::max(0.0f, t));
    const float s = t * m_length;
    // Amostras a passo constante: o segmento sai da divisao, sem busca.
    const int segs = segmentCount();
    const float spacing = m_length / static_cast<float>(m_closed ? n : n - 1);
    int seg = std::min(segs - 1, static_cast<int>(s / spacing));
    const RibbonSample& a = m_samples[static_cast<size_t>(seg)];
    const size_t bi = (static_cast<size_t>(seg) + 1) % n;
    const RibbonSample& b = m_samples[bi];
    const float bs = bi == 0 ? m_length : b.s;
    const float u = bs > a.s ? std::min(1.0f, std::max(0.0f, (s - a.s) / (bs - a.s))) : 0.0f;
    float tx = a.tx + (b.tx - a.tx) * u, tz = a.tz + (b.tz - a.tz) * u;
    const float tl = std::sqrt(tx * tx + tz * tz);
    if (tl > 0.0f) {
        tx /= tl;
        tz /= tl;
    }
    q.ok = true;
    q.segment = seg;
    q.t = t;
    q.s = s;
    q.width = a.width + (b.width - a.width) * u;
    q.edge = q.width * 0.5f;
    q.tx = tx;
    q.tz = tz;
    q.x = a.x + (b.x - a.x) * u;
    q.z = a.z + (b.z - a.z) * u;
    q.height = heightAt(static_cast<size_t>(seg), bi, u, 0.5f);
    return q;
}

void Ribbon::buildMesh(const std::function<float(float, float)>& height, float lift,
                       std::vector<RibbonVertex>& vertices, std::vector<uint32_t>& indices) {
    vertices.clear();
    indices.clear();
    if (m_samples.empty()) return;
    float wMax = 0.0f;
    for (const RibbonSample& r : m_samples) wMax = std::max(wMax, r.width);
    m_cols = std::max(2, static_cast<int>(std::ceil(wMax / m_step)) + 1);
    const size_t n = m_samples.size();
    const size_t cols = static_cast<size_t>(m_cols);
    m_heights.assign(n * cols, 0.0f);
    // Fechada: a primeira fileira se repete no fim com v = comprimento, para a
    // textura nao saltar na emenda.
    const size_t rows = m_closed ? n + 1 : n;
    vertices.resize(rows * cols);
    for (size_t r = 0; r < rows; ++r) {
        const RibbonSample& s = m_samples[r % n];
        const float v = (r == n ? m_length : s.s) / m_uvLength;
        const float rx = s.tz, rz = -s.tx;
        for (size_t c = 0; c < cols; ++c) {
            const float f = static_cast<float>(c) / static_cast<float>(cols - 1);
            const float lat = (f - 0.5f) * s.width;
            const float x = s.x + rx * lat, z = s.z + rz * lat;
            RibbonVertex& o = vertices[r * cols + c];
            if (r < n) m_heights[r * cols + c] = height(x, z) + lift;
            o.px = x;
            o.py = m_heights[(r % n) * cols + c];
            o.pz = z;
            o.u = f;
            o.v = v;
        }
    }
    // Normal pela diferenca central na grade (fileira = ao longo, coluna = de lado).
    for (size_t r = 0; r < rows; ++r)
        for (size_t c = 0; c < cols; ++c) {
            auto at = [&](size_t rr, size_t cc) -> const RibbonVertex& { return vertices[rr * cols + cc]; };
            const size_t c0 = c > 0 ? c - 1 : c, c1 = c + 1 < cols ? c + 1 : c;
            size_t r0 = r > 0 ? r - 1 : r, r1 = r + 1 < rows ? r + 1 : r;
            if (m_closed && r == 0) r0 = n - 1;
            if (m_closed && r == rows - 1) r1 = 1;
            const RibbonVertex &xa = at(r, c0), &xb = at(r, c1), &za = at(r0, c), &zb = at(r1, c);
            const float sx = xb.px - xa.px, sy = xb.py - xa.py, sz = xb.pz - xa.pz;
            const float ax = zb.px - za.px, ay = zb.py - za.py, az = zb.pz - za.pz;
            // lado x ao longo: para cima com lado = direita e ao longo = frente.
            float nx = sy * az - sz * ay, ny = sz * ax - sx * az, nz = sx * ay - sy * ax;
            if (ny < 0.0f) {
                nx = -nx;
                ny = -ny;
                nz = -nz;
            }
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            RibbonVertex& o = vertices[r * cols + c];
            o.nx = len > 0.0f ? nx / len : 0.0f;
            o.ny = len > 0.0f ? ny / len : 1.0f;
            o.nz = len > 0.0f ? nz / len : 0.0f;
        }
    // Mesma ordem do chao gerado (a, c, b / b, c, d com coluna em x e fileira em z).
    indices.reserve((rows - 1) * (cols - 1) * 6);
    for (size_t r = 0; r + 1 < rows; ++r)
        for (size_t c = 0; c + 1 < cols; ++c) {
            const uint32_t a = static_cast<uint32_t>(r * cols + c), b = a + 1;
            const uint32_t cc = a + static_cast<uint32_t>(cols), d = cc + 1;
            indices.insert(indices.end(), {a, cc, b, b, cc, d});
        }
}

uint64_t Ribbon::hash() const {
    Fnv f;
    for (const RibbonSample& s : m_samples) {
        f.add(&s.x, sizeof(float));
        f.add(&s.z, sizeof(float));
        f.add(&s.tx, sizeof(float));
        f.add(&s.tz, sizeof(float));
        f.add(&s.width, sizeof(float));
        f.add(&s.s, sizeof(float));
    }
    if (!m_heights.empty()) f.add(m_heights.data(), m_heights.size() * sizeof(float));
    return f.h;
}

} // namespace eruption
