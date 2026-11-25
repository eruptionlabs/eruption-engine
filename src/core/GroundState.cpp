// Logica pura do estado do chao por celula (ver GroundState.hpp).
#include "core/GroundState.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace eruption::ground {

namespace {

float smooth01(float a, float b, float x) {
    if (b <= a) return x >= b ? 1.0f : 0.0f;
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

uint8_t q8(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

// Fase do degelo 0..1 (0 enquanto segura), a partir de hold = duration - fade.
float meltPhase(const State& s, double now) {
    const float age = float(now - s.born);
    const float hold = std::max(s.duration - s.fade, 0.0f);
    if (age <= hold) return 0.0f;
    return std::clamp((age - hold) / std::max(s.fade, 1e-3f), 0.0f, 1.0f);
}

} // namespace

bool kindFromName(const std::string& name, Kind& out) {
    static const struct { const char* n; Kind k; } kTab[] = {
        {"scorch", Kind::Scorch}, {"frost", Kind::Frost}, {"wet", Kind::Wet},
        {"snow", Kind::Snow},     {"ice_water", Kind::IceWater}};
    for (const auto& e : kTab)
        if (name == e.n) {
            out = e.k;
            return true;
        }
    return false;
}

const char* kindName(Kind k) {
    switch (k) {
    case Kind::Scorch: return "scorch";
    case Kind::Frost: return "frost";
    case Kind::Wet: return "wet";
    case Kind::Snow: return "snow";
    case Kind::IceWater: return "ice_water";
    }
    return "scorch";
}

float lifeAmount(const State& s, double now) {
    const float age = float(now - s.born);
    if (age < 0.0f || age >= s.duration) return 0.0f;
    const float ramp = smooth01(0.0f, 0.5f, age);
    const float phase = meltPhase(s, now);
    return s.strength * ramp * (1.0f - smooth01(0.0f, 1.0f, phase));
}

float meltWet(const State& s, double now) {
    if (s.kind != Kind::Frost && s.kind != Kind::Snow && s.kind != Kind::IceWater) return 0.0f;
    const float t = meltPhase(s, now);
    if (t <= 0.0f || t >= 1.0f) return 0.0f;
    // Molha logo que o gelo cede e vai secando ate' o fim.
    return s.strength * 0.85f * smooth01(0.0f, 0.2f, t) * (1.0f - smooth01(0.3f, 1.0f, t));
}

float profile(const State& s, float px, float pz) {
    const float dx = px - s.x, dz = pz - s.z;
    if (s.square) {
        const float d = std::max(std::fabs(dx), std::fabs(dz));
        return 1.0f - smooth01(0.8f * s.radius, s.radius, d);
    }
    const float d = std::sqrt(dx * dx + dz * dz);
    return 1.0f - smooth01(0.45f * s.radius, s.radius, d);
}

float groundAt(const State& s, float px, float pz) {
    if (s.nx < 2 || s.nz < 2 || s.heights.size() < size_t(s.nx) * size_t(s.nz)) return s.y;
    const float gx = std::clamp((px - s.x0) / s.step, 0.0f, float(s.nx - 1));
    const float gz = std::clamp((pz - s.z0) / s.step, 0.0f, float(s.nz - 1));
    const int ix = std::min(int(gx), s.nx - 2), iz = std::min(int(gz), s.nz - 2);
    const float fx = gx - float(ix), fz = gz - float(iz);
    const float* h = s.heights.data();
    const float a = h[iz * s.nx + ix], b = h[iz * s.nx + ix + 1];
    const float c = h[(iz + 1) * s.nx + ix], d = h[(iz + 1) * s.nx + ix + 1];
    return (a + (b - a) * fx) * (1.0f - fz) + (c + (d - c) * fx) * fz;
}

bool expired(const State& s, double now) { return float(now - s.born) >= s.duration; }

void Rect::add(const Rect& r) {
    if (!r.valid()) return;
    if (!valid()) {
        *this = r;
        return;
    }
    x0 = std::min(x0, r.x0);
    y0 = std::min(y0, r.y0);
    x1 = std::max(x1, r.x1);
    y1 = std::max(y1, r.y1);
}

void Field::configure(const Params& p) {
    Params q = p;
    q.texels = std::clamp<uint32_t>(q.texels, 64u, 1024u);
    q.texelWorld = std::max(q.texelWorld, 1e-3f);
    const bool resized = q.texels != m_p.texels || q.texelWorld != m_p.texelWorld || m_px.empty();
    m_p = q;
    if (resized) {
        m_px.assign(size_t(m_p.texels) * 2 * m_p.texels * 4, 0);
        m_haveWindow = false;
        m_prevDirty = Rect();
        m_dirtyLeft = false;
    }
}

uint32_t Field::add(State s, uint32_t id, const std::function<float(float, float)>& groundY, uint32_t cap) {
    cap = std::clamp<uint32_t>(cap, 1u, kMaxStates);
    while (m_states.size() >= cap) m_states.erase(m_states.begin());
    s.id = id;
    s.radius = std::max(s.radius, 1e-3f);
    s.strength = std::clamp(s.strength, 0.0f, 1.0f);
    s.duration = std::max(s.duration, 0.1f);
    s.fade = std::clamp(s.fade, 0.0f, s.duration);
    if (groundY) {
        // Grade grossa em volta: celulas de 2 texels ou mais, no maximo 39 x 39.
        const float span = 2.0f * s.radius;
        const int n = std::clamp(int(std::ceil(span / (2.0f * m_p.texelWorld))) + 1, 2, 39);
        s.nx = s.nz = n;
        s.step = span / float(n - 1);
        s.x0 = s.x - s.radius;
        s.z0 = s.z - s.radius;
        s.heights.resize(size_t(n) * size_t(n));
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                s.heights[size_t(j) * size_t(n) + size_t(i)] = groundY(s.x0 + float(i) * s.step, s.z0 + float(j) * s.step);
        s.y = groundAt(s, s.x, s.z);
    }
    m_states.push_back(std::move(s));
    countIce();
    return id;
}

bool Field::stop(uint32_t id) {
    for (size_t i = 0; i < m_states.size(); ++i)
        if (m_states[i].id == id) {
            m_states.erase(m_states.begin() + long(i));
            countIce();
            return true;
        }
    return false;
}

void Field::clear() {
    m_states.clear();
    m_iceLive = 0;
    m_iceGrid.clear();
}

namespace {
uint64_t iceKey(int cx, int cz) { return (uint64_t(uint32_t(cx)) << 32) | uint64_t(uint32_t(cz)); }
int iceCell(float v) { return int(std::floor(v / kIceBucket)); }
} // namespace

void Field::countIce() {
    m_iceLive = 0;
    m_iceGrid.clear();
    for (size_t i = 0; i < m_states.size(); ++i) {
        const State& s = m_states[i];
        if (s.kind != Kind::IceWater || !s.walkable) continue;
        ++m_iceLive;
        for (int cz = iceCell(s.z - s.radius); cz <= iceCell(s.z + s.radius); ++cz)
            for (int cx = iceCell(s.x - s.radius); cx <= iceCell(s.x + s.radius); ++cx)
                m_iceGrid[iceKey(cx, cz)].push_back(uint16_t(i));
    }
}

bool Field::animating(double now) const {
    for (const State& s : m_states) {
        const double age = now - s.born;
        if (age < 0.7) return true;
        if (age > double(s.duration - s.fade) - 0.3) return true;
        if (s.kind == Kind::Frost || s.kind == Kind::Snow || s.kind == Kind::IceWater) return true; // derretendo molha
    }
    return false;
}

bool Field::tick(double now) {
    m_now = now;
    const size_t before = m_states.size();
    m_states.erase(std::remove_if(m_states.begin(), m_states.end(), [now](const State& s) { return expired(s, now); }),
                   m_states.end());
    // A trava do gelo (histerese sobre a forca do estado).
    for (State& s : m_states) {
        if (s.kind != Kind::IceWater || !s.walkable) continue;
        const float a = lifeAmount(s, now);
        if (!s.solid && a >= kIceSolidOn) s.solid = s.everSolid = true;
        else if (s.solid && a < kIceSolidOff) s.solid = false;
    }
    if (m_states.size() != before) countIce();
    return m_states.size() != before;
}

float Field::iceLift(float x, float z) const {
    if (m_iceLive == 0) return 0.0f;
    float lift = 0.0f;
    const auto it = m_iceGrid.find(iceKey(iceCell(x), iceCell(z)));
    if (it == m_iceGrid.end()) return 0.0f;
    for (const uint16_t idx : it->second) {
        const State& s = m_states[idx];
        if (std::fabs(x - s.x) > s.radius || std::fabs(z - s.z) > s.radius) continue;
        const float v = lifeAmount(s, m_now) * profile(s, x, z);
        if (v >= (s.solid ? kIceSolidOff : kIceSolidOn)) return 1.0f;
        if (s.everSolid) lift = std::max(lift, smooth01(kIceSinkLow, kIceSolidOff, v));
    }
    return lift;
}

bool Field::follow(float fx, float fz) {
    const float size = windowSize();
    const float snap = 32.0f * m_p.texelWorld;
    if (m_haveWindow) {
        const float cx = m_ox + 0.5f * size, cz = m_oz + 0.5f * size;
        if (std::fabs(fx - cx) < 0.3f * size && std::fabs(fz - cz) < 0.3f * size) return false;
    }
    const float nx = std::floor((fx - 0.5f * size) / snap) * snap;
    const float nz = std::floor((fz - 0.5f * size) / snap) * snap;
    const bool changed = !m_haveWindow || nx != m_ox || nz != m_oz;
    m_ox = nx;
    m_oz = nz;
    m_haveWindow = true;
    if (changed) { // tudo o que estava gravado vale outra coisa agora: limpa a janela inteira
        m_prevDirty = Rect{0, 0, int(m_p.texels) - 1, int(m_p.texels) - 1};
    }
    return changed;
}

Rect Field::rasterize(double now) {
    const int W = int(m_p.texels);
    const float tw = m_p.texelWorld;
    if (m_px.size() != size_t(W) * 2 * size_t(W) * 4) m_px.assign(size_t(W) * 2 * size_t(W) * 4, 0);

    // Caixas das marcas vivas, em celulas, e a faixa de alturas.
    struct Box { const State* s; Rect r; float a, melt; };
    std::vector<Box> boxes;
    boxes.reserve(m_states.size());
    Rect cur;
    float hMin = 1e30f, hMax = -1e30f;
    for (const State& s : m_states) {
        const float a = lifeAmount(s, now), melt = meltWet(s, now);
        if (a <= 0.002f && melt <= 0.002f) continue;
        const int bx0 = int(std::floor((s.x - s.radius - m_ox) / tw)) - 1, bx1 = int(std::floor((s.x + s.radius - m_ox) / tw)) + 1;
        const int by0 = int(std::floor((s.z - s.radius - m_oz) / tw)) - 1, by1 = int(std::floor((s.z + s.radius - m_oz) / tw)) + 1;
        if (bx1 < 0 || by1 < 0 || bx0 >= W || by0 >= W) continue;
        Rect r{std::max(bx0, 0), std::max(by0, 0), std::min(bx1, W - 1), std::min(by1, W - 1)};
        boxes.push_back({&s, r, a, melt});
        cur.add(r);
        if (s.heights.empty()) {
            hMin = std::min(hMin, s.y);
            hMax = std::max(hMax, s.y);
        } else {
            for (float h : s.heights) {
                hMin = std::min(hMin, h);
                hMax = std::max(hMax, h);
            }
        }
    }
    Rect dirty = m_prevDirty;
    dirty.add(cur);
    if (!dirty.valid()) {
        m_dirtyLeft = false;
        return dirty;
    }
    if (hMin > hMax) { hMin = 0.0f; hMax = 0.0f; }
    m_hBase = hMin - 4.0f;
    m_hRange = std::max(hMax - hMin + 8.0f, 16.0f);

    for (int y = dirty.y0; y <= dirty.y1; ++y) {
        const size_t rowL = (size_t(y) * size_t(W) * 2 + size_t(dirty.x0)) * 4;
        std::memset(&m_px[rowL], 0, size_t(dirty.x1 - dirty.x0 + 1) * 4);
        const size_t rowR = (size_t(y) * size_t(W) * 2 + size_t(W) + size_t(dirty.x0)) * 4;
        std::memset(&m_px[rowR], 0, size_t(dirty.x1 - dirty.x0 + 1) * 4);
    }
    for (const Box& b : boxes) {
        const State& s = *b.s;
        const int ch = s.kind == Kind::Scorch ? 0 : (s.kind == Kind::Frost || s.kind == Kind::IceWater) ? 1
                       : s.kind == Kind::Wet ? 2 : 3;
        for (int y = b.r.y0; y <= b.r.y1; ++y) {
            const float wz = m_oz + (float(y) + 0.5f) * tw;
            for (int x = b.r.x0; x <= b.r.x1; ++x) {
                const float wx = m_ox + (float(x) + 0.5f) * tw;
                const float p = profile(s, wx, wz);
                if (p <= 0.0f) continue;
                const float v = b.a * p, wv = b.melt * p;
                if (v <= 0.002f && wv <= 0.002f) continue;
                uint8_t* L = &m_px[(size_t(y) * size_t(W) * 2 + size_t(x)) * 4];
                L[ch] = std::max(L[ch], q8(v));
                if (wv > 0.0f) L[2] = std::max(L[2], q8(wv));
                const float h = groundAt(s, wx, wz);
                const uint32_t h16 = uint32_t(std::lround(std::clamp((h - m_hBase) / m_hRange, 0.0f, 1.0f) * 65535.0f));
                uint8_t* R = &m_px[(size_t(y) * size_t(W) * 2 + size_t(W) + size_t(x)) * 4];
                R[0] = uint8_t(h16 >> 8);
                R[1] = uint8_t(h16 & 255u);
                R[2] = 0;
                R[3] = 255;
            }
        }
    }
    m_prevDirty = cur;
    m_dirtyLeft = cur.valid();
    return dirty;
}

uint32_t Field::iceAreas(double now, float out[][4], uint32_t cap) const {
    uint32_t n = 0;
    for (const State& s : m_states) {
        if (s.kind != Kind::IceWater || n >= cap) continue;
        const float a = lifeAmount(s, now);
        if (a <= 0.01f) continue;
        out[n][0] = s.x;
        out[n][1] = s.z;
        out[n][2] = s.square ? s.radius * 1.2f : s.radius;
        out[n][3] = a;
        ++n;
    }
    return n;
}

} // namespace eruption::ground
