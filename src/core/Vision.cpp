#include "core/Vision.hpp"

#include <algorithm>
#include <cmath>

namespace eruption {

namespace {

float soften(float d, float radius, float edge) {
    // 1 dentro, 0 fora, smoothstep centrado no raio: o raio e' a linha de 50%.
    const float t = std::clamp((d - (radius - 0.5f * edge)) / edge, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

float dist2(const Vec2& a, const Vec2& b) {
    const Vec2 d = a - b;
    return d.x * d.x + d.y * d.y;
}

} // namespace

void Vision::setDim(float dim) {
    m_dim = std::clamp(dim, 0.05f, 1.0f);
}

void Vision::setWindow(float size, uint32_t texels) {
    m_window = std::max(size, 16.0f);
    m_res = std::clamp(texels, 16u, 1024u);
    m_hasHistory = false;
}

void Vision::setSource(const std::string& id, const Vec2& pos, float radius, int team) {
    const Source s{pos, std::max(radius, 0.0f), team, blockZoneAt(pos)};
    auto it = m_sourceIndex.find(id);
    if (it != m_sourceIndex.end()) {
        m_sources[it->second] = s;
        return;
    }
    m_sourceIndex.emplace(id, m_sources.size());
    m_sources.push_back(s);
    m_sourceIds.push_back(id);
}

bool Vision::removeSource(const std::string& id) {
    auto it = m_sourceIndex.find(id);
    if (it == m_sourceIndex.end()) return false;
    const size_t i = it->second, last = m_sources.size() - 1;
    m_sourceIndex.erase(it);
    if (i != last) {
        m_sources[i] = m_sources[last];
        m_sourceIds[i] = std::move(m_sourceIds[last]);
        m_sourceIndex[m_sourceIds[i]] = i;
    }
    m_sources.pop_back();
    m_sourceIds.pop_back();
    return true;
}

void Vision::setZone(const std::string& id, const Vec2& pos, float radius, bool reveal) {
    const Zone z{pos, std::max(radius, 0.0f), reveal};
    auto it = m_zoneIndex.find(id);
    if (it != m_zoneIndex.end()) {
        m_zones[it->second] = z;
    } else {
        m_zoneIndex.emplace(id, m_zones.size());
        m_zones.push_back(z);
        m_zoneIds.push_back(id);
    }
    refreshSourceZones();
}

bool Vision::removeZone(const std::string& id) {
    auto it = m_zoneIndex.find(id);
    if (it == m_zoneIndex.end()) return false;
    const size_t i = it->second, last = m_zones.size() - 1;
    m_zoneIndex.erase(it);
    if (i != last) {
        m_zones[i] = m_zones[last];
        m_zoneIds[i] = std::move(m_zoneIds[last]);
        m_zoneIndex[m_zoneIds[i]] = i;
    }
    m_zones.pop_back();
    m_zoneIds.pop_back();
    refreshSourceZones();
    return true;
}

void Vision::reset() {
    // A janela vem do preset, nao do script: sobrevive.
    const float window = m_window;
    const uint32_t res = m_res;
    *this = Vision();
    m_window = window;
    m_res = res;
}

int Vision::blockZoneAt(const Vec2& p) const {
    for (size_t i = 0; i < m_zones.size(); ++i) {
        const Zone& z = m_zones[i];
        if (!z.reveal && dist2(p, z.pos) < z.radius * z.radius) return static_cast<int>(i);
    }
    return -1;
}

void Vision::refreshSourceZones() {
    for (Source& s : m_sources) s.zone = blockZoneAt(s.pos);
}

float Vision::edgeWidth(float radius, float texel) const {
    // Borda de 8% do raio (a spec pede >= 4%), nunca abaixo de 1,5 texel: com
    // menos o degrau da grade aparece na tela.
    return std::max(0.08f * radius, 1.5f * texel);
}

bool Vision::visible(const Vec2& p) const {
    if (!m_enabled) return true;
    for (const Zone& z : m_zones)
        if (z.reveal && dist2(p, z.pos) < z.radius * z.radius) return true;
    const int zone = blockZoneAt(p);
    for (const Source& s : m_sources) {
        if (s.team != m_team || dist2(p, s.pos) >= s.radius * s.radius) continue;
        if (zone < 0 || s.zone == zone) return true;
    }
    return false;
}

void Vision::rasterTarget(const Vec2& origin, float texel, uint32_t n, std::vector<float>& target,
                          std::vector<int16_t>& zoneMap) const {
    const size_t count = static_cast<size_t>(n) * n;
    target.assign(count, 0.0f);

    // Zona block de cada texel (borda dura; a amostra bilinear suaviza um texel).
    const bool anyBlock = std::any_of(m_zones.begin(), m_zones.end(), [](const Zone& z) { return !z.reveal; });
    if (anyBlock) {
        zoneMap.assign(count, -1);
        for (size_t zi = m_zones.size(); zi-- > 0;) {
            const Zone& z = m_zones[zi];
            if (z.reveal) continue;
            const int x0 = std::max(0, static_cast<int>(std::floor((z.pos.x - z.radius - origin.x) / texel)));
            const int x1 = std::min(static_cast<int>(n) - 1, static_cast<int>(std::floor((z.pos.x + z.radius - origin.x) / texel)));
            const int y0 = std::max(0, static_cast<int>(std::floor((z.pos.y - z.radius - origin.y) / texel)));
            const int y1 = std::min(static_cast<int>(n) - 1, static_cast<int>(std::floor((z.pos.y + z.radius - origin.y) / texel)));
            const float r2 = z.radius * z.radius;
            for (int y = y0; y <= y1; ++y) {
                const float cz = origin.y + (static_cast<float>(y) + 0.5f) * texel - z.pos.y;
                for (int x = x0; x <= x1; ++x) {
                    const float cx = origin.x + (static_cast<float>(x) + 0.5f) * texel - z.pos.x;
                    // De tras para a frente: a primeira zona da lista vence, como no blockZoneAt.
                    if (cx * cx + cz * cz < r2) zoneMap[static_cast<size_t>(y) * n + x] = static_cast<int16_t>(zi);
                }
            }
        }
    }

    auto stamp = [&](const Vec2& pos, float radius, int zone, bool checkZone) {
        const float edge = edgeWidth(radius, texel);
        const float reach = radius + 0.5f * edge;
        const int x0 = std::max(0, static_cast<int>(std::floor((pos.x - reach - origin.x) / texel)));
        const int x1 = std::min(static_cast<int>(n) - 1, static_cast<int>(std::floor((pos.x + reach - origin.x) / texel)));
        const int y0 = std::max(0, static_cast<int>(std::floor((pos.y - reach - origin.y) / texel)));
        const int y1 = std::min(static_cast<int>(n) - 1, static_cast<int>(std::floor((pos.y + reach - origin.y) / texel)));
        // Fora da janela na horizontal: sem isto, numa linha sem miolo o
        // trecho "vazio" (x1 + 1 .. x0 - 1) virava cheio com x1 < x0 e o fill
        // escrevia antes da linha (antes do buffer na linha 0): o heap
        // corrompido derrubava o jogo depois, em outro lugar.
        if (x0 > x1) return;
        // Por linha: o miolo (dentro do raio interno) vira um trecho de 1 sem
        // conta; raiz e smoothstep so' nos texels da borda. 200 fontes de raio
        // 40 numa janela de 1024 u: 0,25 ms -> medido no teste.
        const float inner = std::max(radius - 0.5f * edge, 0.0f);
        const float inner2 = inner * inner, reach2 = reach * reach;
        for (int y = y0; y <= y1; ++y) {
            const float cz = origin.y + (static_cast<float>(y) + 0.5f) * texel - pos.y;
            const float cz2 = cz * cz;
            if (cz2 >= reach2) continue;
            float* row = &target[static_cast<size_t>(y) * n];
            const int16_t* zrow = checkZone ? &zoneMap[static_cast<size_t>(y) * n] : nullptr;
            // Texels com centro dentro do miolo: |cx| < half.
            const float half = cz2 < inner2 ? std::sqrt(inner2 - cz2) : -1.0f;
            const int m0 = half > 0.0f ? std::max(x0, static_cast<int>(std::ceil((pos.x - half - origin.x) / texel - 0.5f))) : x1 + 1;
            const int m1 = half > 0.0f ? std::min(x1, static_cast<int>(std::floor((pos.x + half - origin.x) / texel - 0.5f))) : x0 - 1;
            auto edgeTexel = [&](int x) {
                if (row[x] >= 1.0f || (zrow && zrow[x] >= 0 && zrow[x] != zone)) return;
                const float cx = origin.x + (static_cast<float>(x) + 0.5f) * texel - pos.x;
                const float d2 = cx * cx + cz2;
                if (d2 < reach2) row[x] = std::max(row[x], soften(std::sqrt(d2), radius, edge));
            };
            if (m0 > m1) {
                for (int x = x0; x <= x1; ++x) edgeTexel(x);
                continue;
            }
            for (int x = x0; x < m0; ++x) edgeTexel(x);
            if (zrow) {
                for (int x = m0; x <= m1; ++x)
                    if (zrow[x] < 0 || zrow[x] == zone) row[x] = 1.0f;
            } else {
                std::fill(row + m0, row + m1 + 1, 1.0f);
            }
            for (int x = m1 + 1; x <= x1; ++x) edgeTexel(x);
        }
    };
    for (const Source& s : m_sources)
        if (s.team == m_team && s.radius > 0.0f) stamp(s.pos, s.radius, s.zone, anyBlock);
    for (const Zone& z : m_zones)
        if (z.reveal && z.radius > 0.0f) stamp(z.pos, z.radius, -1, false);
}

void Vision::raster(const Vec2& center, float dt) {
    const uint32_t n = m_res;
    const float texel = m_window / static_cast<float>(n);
    const Vec2 origin(std::floor((center.x - 0.5f * m_window) / texel) * texel,
                      std::floor((center.y - 0.5f * m_window) / texel) * texel);
    const size_t count = static_cast<size_t>(n) * n;
    rasterTarget(origin, texel, n, m_target, m_zoneMap);

    // Meia-vida: a janela andou d texels, entao o historico anda junto.
    const int dx = static_cast<int>(std::lround((origin.x - m_origin.x) / texel));
    const int dy = static_cast<int>(std::lround((origin.y - m_origin.y) / texel));
    if (!m_hasHistory || m_current.size() != count || std::abs(dx) >= static_cast<int>(n) ||
        std::abs(dy) >= static_cast<int>(n) || m_fade <= 0.0f) {
        m_current = m_target;
    } else {
        const float k = 1.0f - std::exp2(-std::max(dt, 0.0f) / m_fade);
        std::vector<float> prev;
        if (dx != 0 || dy != 0) {
            prev.swap(m_current);
            m_current.resize(count);
        }
        for (uint32_t y = 0; y < n; ++y) {
            const int sy = static_cast<int>(y) + dy;
            for (uint32_t x = 0; x < n; ++x) {
                const size_t i = static_cast<size_t>(y) * n + x;
                float cur;
                if (prev.empty()) {
                    cur = m_current[i];
                } else {
                    const int sx = static_cast<int>(x) + dx;
                    const bool inside = sx >= 0 && sy >= 0 && sx < static_cast<int>(n) && sy < static_cast<int>(n);
                    cur = inside ? prev[static_cast<size_t>(sy) * n + sx] : m_target[i];
                }
                m_current[i] = cur + (m_target[i] - cur) * k;
            }
        }
    }
    m_origin = origin;
    m_hasHistory = true;

    m_image.resize(count);
    for (size_t i = 0; i < count; ++i)
        m_image[i] = static_cast<uint8_t>(std::clamp(m_current[i], 0.0f, 1.0f) * 255.0f + 0.5f);
}

void Vision::rasterShade(const Vec2& origin, float size, uint32_t n, const Vec4& keep, std::vector<uint8_t>& out) const {
    const float texel = size / static_cast<float>(n);
    rasterTarget(origin, texel, n, m_mapTarget, m_mapZones);
    out.resize(static_cast<size_t>(n) * n);
    const float dark = 1.0f - m_dim;
    for (uint32_t y = 0; y < n; ++y) {
        const float wz = origin.y + (static_cast<float>(y) + 0.5f) * texel;
        for (uint32_t x = 0; x < n; ++x) {
            const float wx = origin.x + (static_cast<float>(x) + 0.5f) * texel;
            const size_t i = static_cast<size_t>(y) * n + x;
            // Fora do retangulo do mapa a imagem e' transparente: nada a escurecer.
            const bool inside = wx >= keep.x && wz >= keep.y && wx <= keep.z && wz <= keep.w;
            out[i] = inside ? static_cast<uint8_t>(dark * (1.0f - m_mapTarget[i]) * 255.0f + 0.5f) : 0;
        }
    }
}

} // namespace eruption
