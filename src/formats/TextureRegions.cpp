#include "formats/TextureRegions.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

namespace eruption::texregion {

namespace {

constexpr float kPi = 3.14159265358979f;

float lumAt(const uint8_t* img, size_t i, int channels) {
    const uint8_t* p = img + i * static_cast<size_t>(channels);
    if (channels < 3) return p[0] / 255.0f;
    return (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.0f;
}

float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

// Peso do brilho pela luminancia, por modo (ver Glow).
float glowWeight(Glow g, float lum) {
    switch (g) {
        case Glow::Dark: return std::min(1.0f, std::max(0.15f, 1.1f - lum));
        case Glow::Light: return clamp01((lum - 0.5f) / 0.2f);
        case Glow::Paper: return std::min(1.0f, std::max(0.1f, (lum - 0.25f) / 0.45f));
        case Glow::Uniform: return 1.0f;
    }
    return 1.0f;
}

// Desfoque gaussiano separavel, sigma 0,6 (raio 2), borda repetida.
void blur(std::vector<float>& plane, int w, int h) {
    float k[5];
    float sum = 0.0f;
    for (int i = -2; i <= 2; ++i) sum += (k[i + 2] = std::exp(-float(i * i) / (2.0f * 0.6f * 0.6f)));
    for (float& v : k) v /= sum;
    std::vector<float> tmp(plane.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float a = 0.0f;
            for (int i = -2; i <= 2; ++i) a += k[i + 2] * plane[size_t(y) * w + std::min(w - 1, std::max(0, x + i))];
            tmp[size_t(y) * w + x] = a;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float a = 0.0f;
            for (int i = -2; i <= 2; ++i) a += k[i + 2] * tmp[size_t(std::min(h - 1, std::max(0, y + i))) * w + x];
            plane[size_t(y) * w + x] = a;
        }
}

} // namespace

bool isGlowType(const std::string& type) { return type == kTypeWindow || type == kTypeLamp; }
bool isSurfaceType(const std::string& type) { return type == kTypeSand || type == kTypeSnow; }

float luminance(const uint8_t* px) { return (0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2]) / 255.0f; }

bool activeAt(const Region& r, float hour) {
    if (hour < 0.0f || r.onHour < 0.0f || r.offHour < 0.0f) return true;
    if (r.onHour <= r.offHour) return hour >= r.onHour && hour < r.offHour;
    return hour >= r.onHour || hour < r.offHour; // atravessa a meia-noite
}

void rasterize(const Region& r, int w, int h, std::vector<float>& cov) {
    cov.assign(size_t(w) * h, 0.0f);
    switch (r.shape) {
        case Shape::All: std::fill(cov.begin(), cov.end(), 1.0f); break;
        case Shape::Rect: {
            if (r.pts.size() < 4) break;
            const float u0 = std::min(r.pts[0], r.pts[2]), u1 = std::max(r.pts[0], r.pts[2]);
            const float v0 = std::min(r.pts[1], r.pts[3]), v1 = std::max(r.pts[1], r.pts[3]);
            for (int y = 0; y < h; ++y) {
                const float v = (y + 0.5f) / h;
                if (v < v0 || v > v1) continue;
                for (int x = 0; x < w; ++x) {
                    const float u = (x + 0.5f) / w;
                    if (u >= u0 && u <= u1) cov[size_t(y) * w + x] = 1.0f;
                }
            }
            break;
        }
        case Shape::Ellipse: {
            if (r.pts.size() < 4 || r.pts[2] <= 0.0f || r.pts[3] <= 0.0f) break;
            for (int y = 0; y < h; ++y) {
                const float dv = ((y + 0.5f) / h - r.pts[1]) / r.pts[3];
                for (int x = 0; x < w; ++x) {
                    const float du = ((x + 0.5f) / w - r.pts[0]) / r.pts[2];
                    if (du * du + dv * dv <= 1.0f) cov[size_t(y) * w + x] = 1.0f;
                }
            }
            break;
        }
        case Shape::Poly: {
            const size_t n = r.pts.size() / 2;
            if (n < 3) break;
            std::vector<float> xs;
            for (int y = 0; y < h; ++y) {
                const float cy = y + 0.5f;
                xs.clear();
                for (size_t i = 0; i < n; ++i) {
                    const size_t j = (i + 1) % n;
                    const float ay = r.pts[i * 2 + 1] * h, by = r.pts[j * 2 + 1] * h;
                    if ((ay <= cy) == (by <= cy)) continue; // aresta nao cruza esta linha
                    const float t = (cy - ay) / (by - ay);
                    xs.push_back((r.pts[i * 2] + t * (r.pts[j * 2] - r.pts[i * 2])) * w);
                }
                std::sort(xs.begin(), xs.end());
                for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                    const int xa = std::max(0, int(std::ceil(xs[k] - 0.5f)));
                    const int xb = std::min(w - 1, int(std::floor(xs[k + 1] - 0.5f)));
                    for (int x = xa; x <= xb; ++x) cov[size_t(y) * w + x] = 1.0f;
                }
            }
            break;
        }
        case Shape::Bitmap: {
            if (r.bw <= 0 || r.bh <= 0 || r.bitmap.size() < size_t(r.bw) * r.bh) break;
            for (int y = 0; y < h; ++y) {
                const int sy = std::min(r.bh - 1, int((y + 0.5f) * r.bh / h));
                for (int x = 0; x < w; ++x) {
                    const int sx = std::min(r.bw - 1, int((x + 0.5f) * r.bw / w));
                    cov[size_t(y) * w + x] = r.bitmap[size_t(sy) * r.bw + sx] / 255.0f;
                }
            }
            break;
        }
    }
}

void toBitmap(Region& r, int w, int h) {
    if (r.shape == Shape::Bitmap && r.bw > 0) return;
    std::vector<float> cov;
    rasterize(r, w, h, cov);
    r.bitmap.resize(cov.size());
    for (size_t i = 0; i < cov.size(); ++i) r.bitmap[i] = uint8_t(cov[i] * 255.0f + 0.5f);
    r.bw = w;
    r.bh = h;
    r.shape = Shape::Bitmap;
    r.pts.clear();
}

void stamp(Region& r, float cx, float cy, float radiusPx, bool add) {
    if (r.shape != Shape::Bitmap || r.bw <= 0) return;
    const int x0 = std::max(0, int(std::floor(cx - radiusPx - 1))), x1 = std::min(r.bw - 1, int(std::ceil(cx + radiusPx + 1)));
    const int y0 = std::max(0, int(std::floor(cy - radiusPx - 1))), y1 = std::min(r.bh - 1, int(std::ceil(cy + radiusPx + 1)));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
            const float c = clamp01(radiusPx + 0.5f - d);
            if (c <= 0.0f) continue;
            uint8_t& p = r.bitmap[size_t(y) * r.bw + x];
            if (add) p = uint8_t(std::max<float>(p, c * 255.0f));
            else p = uint8_t(std::min<float>(p, (1.0f - c) * 255.0f));
        }
}

void strokeLine(Region& r, float x0, float y0, float x1, float y1, float radiusPx, bool add) {
    const float len = std::hypot(x1 - x0, y1 - y0);
    const int steps = std::max(1, int(len / std::max(0.5f, radiusPx * 0.5f)));
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / steps;
        stamp(r, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, radiusPx, add);
    }
}

void wandFill(const uint8_t* rgba, int w, int h, int channels, int sx, int sy, int tolerance, bool contiguous,
              Region& out) {
    out.shape = Shape::Bitmap;
    out.pts.clear();
    out.bw = w;
    out.bh = h;
    out.bitmap.assign(size_t(w) * h, 0);
    if (sx < 0 || sy < 0 || sx >= w || sy >= h) return;
    const int c = std::min(channels, 3);
    const uint8_t* seed = rgba + (size_t(sy) * w + sx) * channels;
    auto close = [&](int x, int y) {
        const uint8_t* p = rgba + (size_t(y) * w + x) * channels;
        for (int k = 0; k < c; ++k)
            if (std::abs(int(p[k]) - int(seed[k])) > tolerance) return false;
        return true;
    };
    if (!contiguous) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (close(x, y)) out.bitmap[size_t(y) * w + x] = 255;
        return;
    }
    std::vector<int32_t> stack{sy * w + sx};
    out.bitmap[size_t(sy) * w + sx] = 255;
    while (!stack.empty()) {
        const int p = stack.back();
        stack.pop_back();
        const int x = p % w, y = p / w;
        const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
        for (int k = 0; k < 4; ++k) {
            if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
            uint8_t& o = out.bitmap[size_t(ny[k]) * w + nx[k]];
            if (o || !close(nx[k], ny[k])) continue;
            o = 255;
            stack.push_back(ny[k] * w + nx[k]);
        }
    }
}

void buildEmissive(const uint8_t* albedo, int w, int h, int channels, const ImageRegions& img, float hour,
                   std::vector<uint8_t>& rgb) {
    const size_t n = size_t(w) * h;
    std::vector<float> plane[3] = {std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f)};
    std::vector<float> cov;
    bool any = false;
    if (img.enabled && albedo && channels > 0) {
        for (const Region& r : img.regions) {
            if (!isGlowType(r.type) || !activeAt(r, hour)) continue;
            rasterize(r, w, h, cov);
            any = true;
            for (size_t i = 0; i < n; ++i) {
                if (cov[i] <= 0.0f) continue;
                const float m = cov[i] * glowWeight(r.glow, lumAt(albedo, i, channels)) * r.intensity;
                for (int c = 0; c < 3; ++c) plane[c][i] = std::max(plane[c][i], m * r.color[c]);
            }
        }
    }
    rgb.assign(n * 3, 0);
    if (!any) return;
    for (int c = 0; c < 3; ++c) blur(plane[c], w, h);
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = uint8_t(clamp01(plane[c][i]) * 255.0f + 0.5f);
}

std::vector<std::string> surfaceTypes(const ImageRegions& img) {
    std::vector<std::string> out;
    for (const Region& r : img.regions)
        if (isSurfaceType(r.type) && std::find(out.begin(), out.end(), r.type) == out.end()) out.push_back(r.type);
    return out;
}

// ---------------------------------------------------------------- live-wire

void LiveWire::setImage(const uint8_t* rgba, int w, int h, int channels, int maxSide) {
    m_w = m_h = 0;
    m_seed = -1;
    if (!rgba || w <= 0 || h <= 0) return;
    const int s = std::max(1, (std::max(w, h) + maxSide - 1) / maxSide);
    m_w = std::max(1, w / s);
    m_h = std::max(1, h / s);
    // Reducao por media de caixa em cinza: o gradiente de uma textura de 4096
    // so' acha ruido; a 512 acha a borda da janela.
    std::vector<float> g(size_t(m_w) * m_h);
    for (int y = 0; y < m_h; ++y)
        for (int x = 0; x < m_w; ++x) {
            float a = 0.0f;
            int cnt = 0;
            for (int j = 0; j < s; ++j)
                for (int i = 0; i < s; ++i) {
                    const int sx = std::min(w - 1, x * s + i), sy = std::min(h - 1, y * s + j);
                    a += lumAt(rgba, size_t(sy) * w + sx, channels);
                    ++cnt;
                }
            g[size_t(y) * m_w + x] = a / cnt;
        }
    auto at = [&](int x, int y) { return g[size_t(std::min(m_h - 1, std::max(0, y))) * m_w + std::min(m_w - 1, std::max(0, x))]; };
    const size_t n = g.size();
    std::vector<float> gm(n), lap(n);
    m_dx.assign(n, 0.0f);
    m_dy.assign(n, 0.0f);
    float gmax = 1e-6f;
    for (int y = 0; y < m_h; ++y)
        for (int x = 0; x < m_w; ++x) {
            const float gx = (at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1)) -
                             (at(x - 1, y - 1) + 2 * at(x - 1, y) + at(x - 1, y + 1));
            const float gy = (at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1)) -
                             (at(x - 1, y - 1) + 2 * at(x, y - 1) + at(x + 1, y - 1));
            const size_t i = size_t(y) * m_w + x;
            const float mag = std::hypot(gx, gy);
            gm[i] = mag;
            gmax = std::max(gmax, mag);
            if (mag > 1e-6f) { // direcao da borda: perpendicular ao gradiente
                m_dx[i] = gy / mag;
                m_dy[i] = -gx / mag;
            }
            lap[i] = at(x - 1, y) + at(x + 1, y) + at(x, y - 1) + at(x, y + 1) - 4.0f * at(x, y);
        }
    m_fg.resize(n);
    m_fz.assign(n, 1.0f);
    for (size_t i = 0; i < n; ++i) m_fg[i] = 1.0f - gm[i] / gmax;
    // Cruzamento por zero: o laplaciano troca de sinal com o vizinho e a
    // troca e' grande o bastante para nao ser ruido; so' conta onde ha' borda.
    for (int y = 0; y < m_h; ++y)
        for (int x = 0; x < m_w; ++x) {
            const size_t i = size_t(y) * m_w + x;
            if (gm[i] < 0.05f * gmax) continue;
            const int nx[2] = {x + 1, x}, ny[2] = {y, y + 1};
            for (int k = 0; k < 2; ++k) {
                if (nx[k] >= m_w || ny[k] >= m_h) continue;
                const size_t j = size_t(ny[k]) * m_w + nx[k];
                if (lap[i] * lap[j] < 0.0f && std::fabs(lap[i] - lap[j]) > 0.02f) {
                    m_fz[i] = 0.0f;
                    m_fz[j] = std::min(m_fz[j], 0.0f);
                }
            }
        }
}

float LiveWire::linkCost(int p, int q) const {
    const int px = p % m_w, py = p / m_w, qx = q % m_w, qy = q / m_w;
    const bool diag = px != qx && py != qy;
    float lx = float(qx - px), ly = float(qy - py);
    const float len = std::hypot(lx, ly);
    lx /= len;
    ly /= len;
    float dp = m_dx[p] * lx + m_dy[p] * ly;
    float dq = m_dx[q] * lx + m_dy[q] * ly;
    if (dp < 0.0f) { dp = -dp; dq = -dq; } // o sentido do vetor de borda e' arbitrario
    const float fd = (std::acos(std::min(1.0f, dp)) + std::acos(std::max(-1.0f, std::min(1.0f, dq)))) * (2.0f / (3.0f * kPi));
    return 0.43f * m_fz[q] + 0.43f * m_fg[q] * (diag ? 0.70710678f : 1.0f) + 0.14f * fd + 0.001f;
}

void LiveWire::setSeed(float u, float v, int radius) {
    if (m_w <= 0) return;
    const int sx = std::min(m_w - 1, std::max(0, int(u * m_w))), sy = std::min(m_h - 1, std::max(0, int(v * m_h)));
    m_seed = sy * m_w + sx;
    m_x0 = std::max(0, sx - radius);
    m_x1 = std::min(m_w - 1, sx + radius);
    m_y0 = std::max(0, sy - radius);
    m_y1 = std::min(m_h - 1, sy + radius);
    m_dist.assign(size_t(m_w) * m_h, 1e30f);
    m_prev.assign(size_t(m_w) * m_h, -1);
    using Item = std::pair<float, int32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    m_dist[m_seed] = 0.0f;
    q.push({0.0f, m_seed});
    while (!q.empty()) {
        const auto [d, p] = q.top();
        q.pop();
        if (d > m_dist[p]) continue;
        const int x = p % m_w, y = p / m_w;
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                if (!i && !j) continue;
                const int nx = x + i, ny = y + j;
                if (nx < m_x0 || nx > m_x1 || ny < m_y0 || ny > m_y1) continue;
                const int32_t np = ny * m_w + nx;
                const float nd = d + linkCost(p, np);
                if (nd < m_dist[np]) {
                    m_dist[np] = nd;
                    m_prev[np] = p;
                    q.push({nd, np});
                }
            }
    }
}

std::vector<float> LiveWire::pathTo(float u, float v) const {
    std::vector<float> out;
    if (m_seed < 0) return out;
    int x = std::min(m_w - 1, std::max(0, int(u * m_w))), y = std::min(m_h - 1, std::max(0, int(v * m_h)));
    int32_t p = y * m_w + x;
    if (m_dist[p] >= 1e29f) return out;
    while (p >= 0) {
        out.push_back((p % m_w + 0.5f) / m_w);
        out.push_back((p / m_w + 0.5f) / m_h);
        p = m_prev[p];
    }
    // Da semente ate o cursor.
    std::vector<float> rev(out.size());
    for (size_t i = 0; i + 1 < out.size(); i += 2) {
        rev[out.size() - 2 - i] = out[i];
        rev[out.size() - 1 - i] = out[i + 1];
    }
    return rev;
}

} // namespace eruption::texregion
