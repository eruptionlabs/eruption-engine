// Silhueta de contato dos sprites (ver SpriteContact.hpp).
#include "core/SpriteContact.hpp"

#include "formats/SpriteTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <limits>

namespace eruption {

std::shared_ptr<const SpriteContactRows> extractContactRows(const SprImage& img) {
    const size_t w = img.width, h = img.height;
    if (w == 0 || h == 0 || img.pixels.size() < w * h * (img.isIndexed ? 1 : 4)) return nullptr;
    auto opaque = [&](size_t x, size_t y) {
        const size_t i = y * w + x;
        return img.isIndexed ? img.pixels[i] != 0 : img.pixels[i * 4 + 3] >= 128;
    };
    int bottom = -1;
    for (int y = int(h) - 1; y >= 0 && bottom < 0; --y)
        for (size_t x = 0; x < w; ++x)
            if (opaque(x, size_t(y))) { bottom = y; break; }
    if (bottom < 0) return nullptr;
    auto r = std::make_shared<SpriteContactRows>();
    r->width = img.width;
    r->height = img.height;
    r->bottom = bottom;
    r->rows = std::min(SpriteContactRows::kRows, bottom + 1);
    const size_t stride = (w + 7) / 8;
    r->bits.assign(size_t(r->rows) * stride, 0);
    for (int k = 0; k < r->rows; ++k)
        for (size_t x = 0; x < w; ++x)
            if (opaque(x, size_t(bottom - k))) r->bits[size_t(k) * stride + (x >> 3)] |= uint8_t(1u << (x & 7));
    return r;
}

namespace contact {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kTouch = 0.3f;    // uma coluna "pisa" se o pixel mais baixo dela esta' a ate' isto do chao
constexpr float kMinStrike = 0.4f; // mancha mais estreita que isto e' ruido de borda (mundo)
constexpr float kMaxSlide = 2.0f; // o pe' parado anda no desenho ate' isto entre dois quadros (mundo)
constexpr int kMergeGap = 2;
constexpr float kHop = 0.45f;     // o corpo sobe do chao mais que 3 pixels de arte: salta
constexpr float kBodyLand = 0.5f; // e a pisada e' metade do desenho ou mais: o corpo pousa      // buraco de ate' 2 colunas nao separa duas manchas

// Visita cada pixel guardado de cada camada: colunas [ca, cb] da grade do
// quadro (origem x0) e alturas [upLo, upHi] acima da ancora.
template <class Fn>
void eachPixel(const std::vector<Layer>& layers, float x0, int n, Fn&& fn) {
    for (const Layer& l : layers) {
        const SpriteContactRows& r = *l.rows;
        const float left = l.sx - 0.5f * float(r.width) * l.pw;
        for (int k = 0; k < r.rows; ++k) {
            const int imgRow = r.bottom - k;
            const float upLo = l.up0 + float(int(r.height) - 1 - imgRow) * l.ph;
            for (int i = 0; i < int(r.width); ++i) {
                if (!r.at(k, i)) continue;
                const int col = l.mirror ? int(r.width) - 1 - i : i;
                const float xa = left + float(col) * l.pw, xb = xa + l.pw;
                const int ca = std::max(0, int(std::floor((xa - x0) / kCell + 1e-3f)));
                const int cb = std::min(n - 1, int(std::ceil((xb - x0) / kCell - 1e-3f)) - 1);
                fn(ca, std::max(ca, cb), upLo, upLo + l.ph);
            }
        }
    }
}

// Colunas [c0, c1] das corridas de `mask` (buraco de kMergeGap colunas nao separa).
std::vector<std::pair<int, int>> runsOf(const std::vector<uint8_t>& mask) {
    std::vector<std::pair<int, int>> runs;
    const int n = int(mask.size());
    int c = 0;
    while (c < n) {
        if (!mask[size_t(c)]) { ++c; continue; }
        int c1 = c, gap = 0;
        for (int k = c + 1; k < n; ++k) {
            if (mask[size_t(k)]) { c1 = k; gap = 0; }
            else if (++gap > kMergeGap) break;
        }
        runs.emplace_back(c, c1);
        c = c1 + 1;
    }
    return runs;
}

Blob makeBlob(const Frame& f, int c0, int c1) {
    Blob b;
    b.c0 = c0;
    b.c1 = c1;
    b.width = float(c1 - c0 + 1) * kCell;
    b.sxCenter = f.x0 + float(c0 + c1 + 1) * 0.5f * kCell;
    return b;
}

// O que apareceu em `cur` (mascara `now`) em relacao a `before`, do quadro
// anterior. O pe' parado no chao anda no desenho com o corpo (anda para tras
// da arte): a mascara anterior e' deslocada pelo alinhamento que mais coincide
// com a de agora antes de ver o que sobra - o resto e' chao novo: um pe' que
// aparece ou que desce ate' o chao vindo de cima.
std::vector<std::pair<int, int>> freshRuns(const Frame& prev, const std::vector<uint8_t>& before, const Frame& cur,
                                           const std::vector<uint8_t>& now) {
    if (cur.n == 0) return {};
    const int maxShift = int(kMaxSlide / kCell);
    auto downPrev = [&](int c, int shift) {
        // coluna c de cur <-> coluna (c - shift) de prev, passando por mundo
        const float sx = cur.x0 + (float(c) + 0.5f) * kCell - float(shift) * kCell;
        const int p = int(std::floor((sx - prev.x0) / kCell));
        return p >= 0 && p < prev.n && before[size_t(p)];
    };
    int best = 0, bestScore = -1;
    if (prev.n > 0)
        for (int sh = -maxShift; sh <= maxShift; ++sh) {
            int score = 0;
            for (int c = 0; c < cur.n; ++c)
                if (now[size_t(c)] && downPrev(c, sh)) ++score;
            if (score > bestScore || (score == bestScore && std::abs(sh) < std::abs(best))) {
                bestScore = score;
                best = sh;
            }
        }
    std::vector<uint8_t> fresh(size_t(cur.n), 0);
    for (int c = 0; c < cur.n; ++c) fresh[size_t(c)] = now[size_t(c)] && !(prev.n > 0 && downPrev(c, best));
    std::vector<std::pair<int, int>> out;
    for (const auto& r : runsOf(fresh))
        if (float(r.second - r.first + 1) * kCell >= kMinStrike) out.push_back(r);
    return out;
}

} // namespace

void setDrag(Action& a, bool drag) {
    a.drag = drag && a.any;
    // Pisada: quantas manchas separadas tocam o chao de uma vez (2 pes, 4 patas)
    // ou quantas pisadas o ciclo tem; uma so' larga que salta e' uma mancha.
    const int feet = std::max(a.maxRuns, a.strikeCount);
    // Um corpo largo que sobe do chao e pousa inteiro (gelatina) e' uma mancha;
    // pisada estreita: dois pes = sola, o resto (uma pata ou varias) = pata.
    const bool bodyLands = a.hop > kHop && a.landFrac >= kBodyLand;
    a.shape = a.drag ? Shape::Trail : bodyLands ? Shape::Blot : feet == 2 ? Shape::Sole : Shape::Paw;
}

Action build(const std::vector<std::vector<Layer>>& frames) {
    Action a;
    a.frames.resize(frames.size());
    // Passe 1: grade e pixel mais baixo de cada coluna.
    for (size_t fi = 0; fi < frames.size(); ++fi) {
        Frame& f = a.frames[fi];
        float x0 = kInf, x1 = -kInf;
        for (const Layer& l : frames[fi]) {
            if (!l.rows) continue;
            x0 = std::min(x0, l.sx - 0.5f * float(l.rows->width) * l.pw);
            x1 = std::max(x1, l.sx + 0.5f * float(l.rows->width) * l.pw);
        }
        if (x1 <= x0) continue;
        f.x0 = x0;
        f.n = int(std::ceil((x1 - x0) / kCell)) + 1;
        f.low.assign(size_t(f.n), kInf);
        eachPixel(frames[fi], x0, f.n, [&](int ca, int cb, float upLo, float) {
            for (int c = ca; c <= cb; ++c) f.low[size_t(c)] = std::min(f.low[size_t(c)], upLo);
        });
        f.lowest = *std::min_element(f.low.begin(), f.low.end());
    }
    // Passe 2: o chao do quadro (o pixel mais baixo e o que esta' a ate' kTouch
    // dele) e o chao do ciclo.
    float ground = kInf;
    for (const Frame& f : a.frames)
        if (f.n > 0) ground = std::min(ground, f.lowest);
    a.ground = ground;
    for (size_t fi = 0; fi < frames.size(); ++fi) {
        Frame& f = a.frames[fi];
        if (f.n == 0) continue;
        f.touch.assign(size_t(f.n), 0);
        f.touchG.assign(size_t(f.n), 0);
        // Perfil da sola: quanto de pixel opaco fica na faixa [chao, chao + kProfH].
        f.prof.assign(size_t(f.n), 0.0f);
        eachPixel(frames[fi], f.x0, f.n, [&](int ca, int cb, float upLo, float upHi) {
            const float h = std::min(upHi, ground + kProfH) - std::max(upLo, ground);
            if (h > 0.0f)
                for (int c = ca; c <= cb; ++c) f.prof[size_t(c)] += h;
        });
        for (float& v : f.prof) v = std::min(v, kProfH);
        for (int c = 0; c < f.n; ++c) {
            f.touch[size_t(c)] = f.low[size_t(c)] <= f.lowest + kTouch;
            f.touchG[size_t(c)] = f.low[size_t(c)] <= ground + kTouch;
        }
        // Todo o chao do quadro, de ponta a ponta: o arrasto.
        auto runs = runsOf(f.touch);
        std::vector<std::pair<int, int>> keep;
        for (const auto& r : runs)
            if (float(r.second - r.first + 1) * kCell >= kMinStrike) keep.push_back(r);
        if (!keep.empty()) runs = keep;
        if (!runs.empty()) {
            f.all = makeBlob(f, runs.front().first, runs.back().second);
            f.any = true;
            a.any = true;
        }
    }
    // Pisadas: o que de touchG aparece entre quadros do ciclo. Separado, o
    // mesmo teste no chao do PROPRIO quadro diz se o corpo so' desliza: o
    // chao dele persiste e quase nada novo toca (uma cobra), ao contrario de
    // quem tem pe's (varios quadros com pisada) ou salta (o chao some).
    int slideStrikeFrames = 0;
    bool allHaveGround = true;
    const size_t nf = a.frames.size();
    for (size_t fi = 0; fi < nf; ++fi) {
        Frame& f = a.frames[fi];
        const Frame& prev = a.frames[(fi + nf - 1) % nf];
        allHaveGround = allHaveGround && f.any;
        f.strikes.clear();
        if (nf < 2) continue;
        for (const auto& r : freshRuns(prev, prev.touchG, f, f.touchG)) f.strikes.push_back(makeBlob(f, r.first, r.second));
        if (!f.touch.empty() && !freshRuns(prev, prev.touch, f, f.touch).empty()) ++slideStrikeFrames;
    }
    a.slideFrames = slideStrikeFrames;
    a.drag = a.any && allHaveGround && slideStrikeFrames <= 1;
    for (const Frame& f : a.frames) {
        a.strikeCount += int(f.strikes.size());
        a.bodyWidth = std::max(a.bodyWidth, float(f.n) * kCell);
        if (f.n > 0) a.hop = std::max(a.hop, f.lowest - a.ground);
        for (const Blob& b : f.strikes) a.landFrac = std::max(a.landFrac, b.width / std::max(float(f.n) * kCell, 1e-3f));
        int runs = 0;
        for (const auto& r : runsOf(f.touchG))
            if (float(r.second - r.first + 1) * kCell >= kMinStrike) ++runs;
        a.maxRuns = std::max(a.maxRuns, runs);
    }
    setDrag(a, a.drag);
    return a;
}

void setFamily(Action& a, Shape shape) {
    a.shape = shape;
    a.strideFallback = shape == Shape::Sole && a.strikeCount < 2;
}

bool viewProfile(const Action& a, std::vector<float>& prof) {
    // As manchas no chao do ciclo: cada pe' (ou o corpo inteiro no arrasto).
    struct Run { const Frame* f; int c0, c1; };
    std::vector<Run> runs;
    for (const Frame& f : a.frames) {
        if (f.n == 0) continue;
        if (a.drag) {
            if (f.any) runs.push_back({&f, f.all.c0, f.all.c1});
            continue;
        }
        for (const auto& r : runsOf(f.touchG))
            if (float(r.second - r.first + 1) * kCell >= kMinStrike) runs.push_back({&f, r.first, r.second});
    }
    if (runs.empty()) return false;
    // Dois pes juntos na vista (o de tras cruza o da frente) ou a barra de um
    // manto saem largos demais: so' as manchas ate' 1,3x a mediana.
    std::vector<int> widths;
    for (const Run& r : runs) widths.push_back(r.c1 - r.c0 + 1);
    std::nth_element(widths.begin(), widths.begin() + long(widths.size() / 2), widths.end());
    const int med = widths[widths.size() / 2];
    constexpr int kPad = 3; // a ponta do pe' que nao encosta ainda e' sola
    const int half = int(float(med) * 0.8f) + kPad + 4;
    std::vector<float> sum(size_t(2 * half + 1), 0.0f);
    int used = 0;
    for (const Run& r : runs) {
        if (float(r.c1 - r.c0 + 1) > 1.3f * float(med)) continue;
        const Frame& f = *r.f;
        const int a0 = std::max(0, r.c0 - kPad), a1 = std::min(f.n - 1, r.c1 + kPad);
        float w = 0.0f, wc = 0.0f;
        for (int c = a0; c <= a1; ++c) {
            w += f.prof[size_t(c)];
            wc += f.prof[size_t(c)] * (float(c) + 0.5f);
        }
        if (w <= 0.0f) continue;
        const float center = wc / w;
        for (int c = a0; c <= a1; ++c) {
            const int k = int(std::lround(float(c) + 0.5f - center)) + half;
            if (k >= 0 && k < int(sum.size())) sum[size_t(k)] += f.prof[size_t(c)];
        }
        ++used;
    }
    if (used == 0) return false;
    const float peak = *std::max_element(sum.begin(), sum.end());
    if (peak <= 0.0f) return false;
    prof.resize(sum.size());
    for (size_t k = 0; k < sum.size(); ++k) prof[k] = sum[k] / peak;
    return true;
}

namespace {

float smooth01(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v * v * (3.0f - 2.0f * v);
}

float smoothstep(float a, float b, float v) { return smooth01((v - a) / (b - a)); }

// Ruido de valor (grade inteira, interpolado): o grao da terra.
float hashf(int x, int y, uint32_t seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}
float valueNoise(float x, float y, uint32_t seed) {
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = smooth01(x - float(ix)), fy = smooth01(y - float(iy));
    const float a = hashf(ix, iy, seed), b = hashf(ix + 1, iy, seed);
    const float c = hashf(ix, iy + 1, seed), d = hashf(ix + 1, iy + 1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
float grain(float x, float y) {
    return 0.5f * valueNoise(x, y, 1u) + 0.3f * valueNoise(x * 2.1f, y * 2.1f, 2u) + 0.2f * valueNoise(x * 4.3f, y * 4.3f, 3u);
}

// Perfil amostrado em s (mundo, 0 no centro), interpolado; 0 fora.
float sampleProfile(const std::vector<float>& p, float s) {
    const float x = s / kCell + float(p.size()) * 0.5f - 0.5f;
    const int i = int(std::floor(x));
    const float t = x - float(i);
    auto at = [&](int k) { return k >= 0 && k < int(p.size()) ? p[size_t(k)] : 0.0f; };
    return at(i) + (at(i + 1) - at(i)) * t;
}

// Desfoque de caixa separavel, raio r, borda presa.
void boxBlur(std::vector<float>& img, int t, int r) {
    std::vector<float> tmp(img.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < t; ++y)
            for (int x = 0; x < t; ++x) {
                float acc = 0.0f;
                for (int k = -r; k <= r; ++k) {
                    const int xx = std::clamp(pass == 0 ? x + k : x, 0, t - 1);
                    const int yy = std::clamp(pass == 0 ? y : y + k, 0, t - 1);
                    acc += img[size_t(yy) * size_t(t) + size_t(xx)];
                }
                tmp[size_t(y) * size_t(t) + size_t(x)] = acc / float(2 * r + 1);
            }
        img.swap(tmp);
    }
}

} // namespace

namespace {
// Casco das vistas num quadrado de lado `side` (t x t). `occ`: o casco visual
// (o ponto so' fica se as vistas tem pixel de sola sobre ele); `depth`: o
// menor perfil (a sola mais alta no desenho = pisa mais fundo).
void hullGrid(const std::vector<View>& views, float side, int t, std::vector<float>& occ, std::vector<float>& depth) {
    const float texel = side / float(t);
    occ.assign(size_t(t) * size_t(t), 0.0f);
    depth.assign(size_t(t) * size_t(t), 0.0f);
    if (views.empty()) return;
    // Consenso, nao o minimo estrito: a arte nem sempre concorda entre as
    // direcoes (o manto esconde o pe' numa diagonal de tras e ela sai com
    // 0,9 onde as outras dao 2 a 3); as 2 vistas mais estreitas de cada 8
    // nao cortam o casco.
    const size_t skip = views.size() / 4;
    std::vector<float> os(views.size()), ds(views.size());
    for (int ty = 0; ty < t; ++ty)
        for (int tx = 0; tx < t; ++tx) {
            const float u = (float(tx) + 0.5f) * texel - side * 0.5f;
            const float v = side * 0.5f - (float(ty) + 0.5f) * texel; // topo = frente
            for (size_t k = 0; k < views.size(); ++k) {
                const View& w = views[k];
                const float p = sampleProfile(w.prof, u * w.au + v * w.av);
                os[k] = smoothstep(0.02f, 0.15f, p);
                ds[k] = p;
            }
            std::nth_element(os.begin(), os.begin() + long(skip), os.end());
            std::nth_element(ds.begin(), ds.begin() + long(skip), ds.end());
            occ[size_t(ty) * size_t(t) + size_t(tx)] = os[skip];
            depth[size_t(ty) * size_t(t) + size_t(tx)] = ds[skip];
        }
}
} // namespace

Mask hullMask(const std::vector<View>& views, bool trail) {
    Mask m;
    m.trail = trail;
    // Primeiro um quadrado folgado (o maior perfil de qualquer vista) para
    // achar onde o casco esta'; depois o quadrado justo, 1,6x a maior extensao.
    float reach = 0.3f;
    for (const View& v : views) reach = std::max(reach, float(v.prof.size()) * kCell * 0.5f);
    float extent = 0.0f;
    {
        const int t0 = 64;
        const float s0 = 2.0f * reach;
        std::vector<float> c0, d0;
        hullGrid(views, s0, t0, c0, d0);
        for (int ty = 0; ty < t0; ++ty)
            for (int tx = 0; tx < t0; ++tx)
                if (c0[size_t(ty) * size_t(t0) + size_t(tx)] > 0.5f) {
                    const float u = (float(tx) + 0.5f) / float(t0) - 0.5f, v = 0.5f - (float(ty) + 0.5f) / float(t0);
                    extent = std::max(extent, 2.0f * std::max(std::fabs(u), std::fabs(v)) * s0);
                }
    }
    const float side = std::max(1.6f * extent, 0.6f);
    int t = int(std::ceil(side / (0.5f * kCell)));
    t = std::clamp((t + 1) & ~1, 32, 96);
    m.size = t;
    m.side = side;
    const float texel = side / float(t);
    std::vector<float> cov, deep;
    hullGrid(views, side, t, cov, deep);
    // Contorno arredondado (os degraus do pixel de arte somem) e o fundo da
    // pisada, que desce devagar da borda para o miolo.
    // Raio do arredondamento: 16% da extensao (os cantos do casco de 8
    // faixas somem; o pe' fica oval, nao octogono).
    boxBlur(cov, t, std::max(1, int(std::lround(0.16f * extent / texel))));
    boxBlur(deep, t, std::max(1, int(std::lround(0.15f * extent / texel))));
    float peak = 1e-4f, deepPeak = 1e-4f;
    for (float c : cov) peak = std::max(peak, c);
    for (float c : deep) deepPeak = std::max(deepPeak, c);
    m.rgba.assign(size_t(t) * size_t(t) * 4, 0);
    float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
    uint32_t hash = 2166136261u;
    // Escurecimento (luz linear): a terra afundada. 0,7 dava so' 28% mais
    // escuro na tela (mediana, depois da curva de gama) e a pisada sumia num
    // chao de textura forte. O rastro e' carimbado com sobreposicao e
    // escurece menos por carimbo.
    const float kDark = trail ? 0.6f : 0.95f;
    for (int ty = 0; ty < t; ++ty)
        for (int tx = 0; tx < t; ++tx) {
            const size_t i = size_t(ty) * size_t(t) + size_t(tx);
            const float u = (float(tx) + 0.5f) * texel - side * 0.5f;
            const float v = side * 0.5f - (float(ty) + 0.5f) * texel;
            const float g = grain(u / (0.15f * extent), v / (0.15f * extent)); // grao: 15% da marca
            const float c = cov[i] / peak + (g - 0.5f) * 0.22f;  // borda roida pelo grao
            const float edge = smoothstep(0.35f, 0.6f, c);
            const float press = smoothstep(0.1f, 0.8f, deep[i] / deepPeak);
            // A parede da pisada, logo dentro do contorno, na sombra: mais
            // escura que o miolo. Le' o pe' num chao de textura forte.
            const float wall = edge * (1.0f - smoothstep(0.62f, 0.9f, c));
            float a = kDark * (edge * (0.4f + 0.35f * press) + 0.4f * wall) *
                      (0.8f + 0.4f * grain(u / (0.06f * extent) + 7.0f, v / (0.06f * extent)));
            a = std::clamp(a, 0.0f, 1.0f);
            const uint8_t a8 = uint8_t(a * 255.0f + 0.5f);
            // Cor branca com o grao (a `color` do script tinge): por dentro, a
            // terra compactada e lisa, com pouca variacao.
            const uint8_t g8 = uint8_t((0.82f + 0.18f * g) * 255.0f + 0.5f);
            m.rgba[i * 4 + 0] = m.rgba[i * 4 + 1] = m.rgba[i * 4 + 2] = g8;
            m.rgba[i * 4 + 3] = a8;
            if (a8 > 24) { u0 = std::min(u0, u); u1 = std::max(u1, u); v0 = std::min(v0, v); v1 = std::max(v1, v); }
            hash = (hash ^ a8) * 16777619u;
        }
    m.width = u1 > u0 ? u1 - u0 : 0.0f;
    m.length = v1 > v0 ? v1 - v0 : 0.0f;
    hash = (hash ^ uint32_t(t)) * 16777619u;
    hash = (hash ^ uint32_t(side * 100.0f + 0.5f)) * 16777619u;
    hash = (hash ^ uint32_t(trail)) * 16777619u;
    char id[32];
    std::snprintf(id, sizeof(id), "contact:%08x", hash);
    m.id = id;
    return m;
}

} // namespace contact
} // namespace eruption
