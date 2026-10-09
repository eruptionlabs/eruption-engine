#include "utils/HeightFromAlbedo.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <cstddef>

namespace eruption {

namespace {

using Img = std::vector<float>;

inline int wrapi(int v, int n) { v %= n; return v < 0 ? v + n : v; }

float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Box blur separavel com WRAP (textura ladrilha) - soma corrida, O(N).
Img boxWrap(const Img& src, int w, int h, int r) {
    if (r <= 0) return src;
    Img tmp(src.size()), dst(src.size());
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    for (int y = 0; y < h; ++y) {
        const float* row = &src[static_cast<size_t>(y) * w];
        float s = 0.0f;
        for (int x = -r; x <= r; ++x) s += row[wrapi(x, w)];
        for (int x = 0; x < w; ++x) {
            tmp[static_cast<size_t>(y) * w + x] = s * inv;
            s += row[wrapi(x + r + 1, w)] - row[wrapi(x - r, w)];
        }
    }
    for (int x = 0; x < w; ++x) {
        float s = 0.0f;
        for (int y = -r; y <= r; ++y) s += tmp[static_cast<size_t>(wrapi(y, h)) * w + x];
        for (int y = 0; y < h; ++y) {
            dst[static_cast<size_t>(y) * w + x] = s * inv;
            s += tmp[static_cast<size_t>(wrapi(y + r + 1, h)) * w + x]
               - tmp[static_cast<size_t>(wrapi(y - r, h)) * w + x];
        }
    }
    return dst;
}

// Gaussiana ~ 3 passes de box (Wells 1986): largura da box pra um sigma.
Img gaussWrap(const Img& src, int w, int h, float sigma) {
    if (sigma < 0.5f) return src;
    const float wIdeal = std::sqrt(12.0f * sigma * sigma / 3.0f + 1.0f);
    int r = std::max(1, static_cast<int>((wIdeal - 1.0f) * 0.5f + 0.5f));
    r = std::min(r, std::min(w, h) / 2 - 1);
    if (r < 1) return src;
    Img a = boxWrap(src, w, h, r);
    a = boxWrap(a, w, h, r);
    return boxWrap(a, w, h, r);
}

float meanOf(const Img& a) {
    double s = 0.0;
    for (float v : a) s += v;
    return static_cast<float>(s / static_cast<double>(a.size()));
}

float rmsOf(const Img& a) {
    double s = 0.0;
    for (float v : a) s += static_cast<double>(v) * v;
    return static_cast<float>(std::sqrt(s / static_cast<double>(a.size())));
}

// Scharr 3x3 com wrap, normalizado pra gradiente por texel.
void scharr(const Img& src, int w, int h, Img& gx, Img& gy) {
    gx.assign(src.size(), 0.0f);
    gy.assign(src.size(), 0.0f);
    auto at = [&](int x, int y) { return src[static_cast<size_t>(wrapi(y, h)) * w + wrapi(x, w)]; };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float tl = at(x - 1, y - 1), t = at(x, y - 1), tr = at(x + 1, y - 1);
            const float l = at(x - 1, y), r = at(x + 1, y);
            const float bl = at(x - 1, y + 1), b = at(x, y + 1), br = at(x + 1, y + 1);
            const size_t i = static_cast<size_t>(y) * w + x;
            gx[i] = (3.0f * (tr - tl) + 10.0f * (r - l) + 3.0f * (br - bl)) / 32.0f;
            gy[i] = (3.0f * (bl - tl) + 10.0f * (b - t) + 3.0f * (br - tr)) / 32.0f;
        }
    }
}

// FFT radix-2 in-place (Cooley-Tukey), n potencia de 2.
void fft1d(std::complex<float>* a, int n, bool inverse) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = 2.0f * 3.14159265358979f / static_cast<float>(len) * (inverse ? 1.0f : -1.0f);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> wv(1.0f, 0.0f);
            for (int k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * wv;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                wv *= wl;
            }
        }
    }
    if (inverse) {
        const float inv = 1.0f / static_cast<float>(n);
        for (int i = 0; i < n; ++i) a[i] *= inv;
    }
}

void fft2d(std::vector<std::complex<float>>& a, int w, int h, bool inverse) {
    for (int y = 0; y < h; ++y) fft1d(&a[static_cast<size_t>(y) * w], w, inverse);
    std::vector<std::complex<float>> col(static_cast<size_t>(h));
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) col[y] = a[static_cast<size_t>(y) * w + x];
        fft1d(col.data(), h, inverse);
        for (int y = 0; y < h; ++y) a[static_cast<size_t>(y) * w + x] = col[y];
    }
}

int nextPow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }

// Reamostragem bilinear periodica (pra levar o tile a potencia de 2 pra FFT
// e trazer o resultado de volta).
Img resampleWrap(const Img& src, int sw, int sh, int dw, int dh) {
    if (sw == dw && sh == dh) return src;
    Img dst(static_cast<size_t>(dw) * dh);
    for (int y = 0; y < dh; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(sh) / static_cast<float>(dh) - 0.5f;
        const int y0 = static_cast<int>(std::floor(fy));
        const float ty = fy - static_cast<float>(y0);
        for (int x = 0; x < dw; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * static_cast<float>(sw) / static_cast<float>(dw) - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const float tx = fx - static_cast<float>(x0);
            auto at = [&](int xx, int yy) { return src[static_cast<size_t>(wrapi(yy, sh)) * sw + wrapi(xx, sw)]; };
            const float a = at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx;
            const float b = at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx;
            dst[static_cast<size_t>(y) * dw + x] = a * (1 - ty) + b * ty;
        }
    }
    return dst;
}

// FRANKOT-CHELLAPPA: integra (p, q) na superficie z de minimos quadrados.
// Versao consistente com diferenca CENTRAL (o Scharr e' central): o operador
// discreto d/dx tem transformada i*sin(wx), entao
//   Z = (conj(Dx) P + conj(Dy) Q) / (|Dx|^2 + |Dy|^2),  |Dx|^2 = sin^2(wx).
// Periodico (textura ladrilha), DC = 0 (altura definida a menos de constante).
Img frankotChellappa(const Img& p, const Img& q, int w, int h) {
    std::vector<std::complex<float>> P(p.begin(), p.end()), Q(q.begin(), q.end());
    fft2d(P, w, h, false);
    fft2d(Q, w, h, false);
    std::vector<std::complex<float>> Z(P.size());
    const float twoPi = 2.0f * 3.14159265358979f;
    for (int ky = 0; ky < h; ++ky) {
        const float wy = twoPi * static_cast<float>(ky) / static_cast<float>(h);
        const std::complex<float> Dy(0.0f, std::sin(wy));
        for (int kx = 0; kx < w; ++kx) {
            const float wx = twoPi * static_cast<float>(kx) / static_cast<float>(w);
            const std::complex<float> Dx(0.0f, std::sin(wx));
            const float den = std::norm(Dx) + std::norm(Dy);
            const size_t i = static_cast<size_t>(ky) * w + kx;
            Z[i] = den > 1e-6f ? (std::conj(Dx) * P[i] + std::conj(Dy) * Q[i]) / den
                               : std::complex<float>(0.0f, 0.0f);
        }
    }
    fft2d(Z, w, h, true);
    Img z(Z.size());
    for (size_t i = 0; i < Z.size(); ++i) z[i] = Z[i].real();
    return z;
}


// ---------------------------------------------------------------------------
// ESTRUTURA: rede de rejunte por FORMA, nao por brilho.
//
// Rejunte e' uma LINHA FINA entre regioes maiores - escuro no chao de pedra,
// claro na parede de tijolo. Brilho nao decide nada; o que decide e' a forma.
// Detector de linha pela Hessiana em varias escalas (Frangi, Niessen, Vincken
// & Viergever 1998, "Multiscale vessel enhancement filtering", MICCAI): numa
// linha, uma curvatura e' grande (atravessando) e a outra ~0 (ao longo).
// lambda2 > 0 = vale (linha escura), lambda2 < 0 = crista (linha clara).
// A polaridade que forma a REDE dominante da textura e' o rejunte - isso e' o
// "contexto": a decisao e' da textura inteira, nao do pixel.
// ---------------------------------------------------------------------------
void frangiLines(const Img& L, int w, int h, Img& valley, Img& ridge) {
    const size_t n = static_cast<size_t>(w) * h;
    valley.assign(n, 0.0f);
    ridge.assign(n, 0.0f);
    // Ate' 5 texels: junta de tabua e' mais larga que rejunte de pedra.
    const float sigmas[5] = {1.0f, 1.5f, 2.25f, 3.4f, 5.0f};
    const float beta = 0.5f;
    for (float sg : sigmas) {
        Img G = gaussWrap(L, w, h, sg);
        auto at = [&](int x, int y) { return G[static_cast<size_t>(wrapi(y, h)) * w + wrapi(x, w)]; };
        Img l1(n), l2(n), edgeSup(n, 1.0f);
        float sMax = 1e-8f;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float c = at(x, y);
                // Normalizado por escala (sigma^2) - Lindeberg: a resposta de
                // escalas diferentes fica comparavel.
                const float s2 = sg * sg;
                const float lxx = (at(x + 1, y) - 2.0f * c + at(x - 1, y)) * s2;
                const float lyy = (at(x, y + 1) - 2.0f * c + at(x, y - 1)) * s2;
                const float lxy = 0.25f * (at(x + 1, y + 1) - at(x - 1, y + 1) - at(x + 1, y - 1) + at(x - 1, y - 1)) * s2;
                const float tr = lxx + lyy;
                const float d = std::sqrt((lxx - lyy) * (lxx - lyy) + 4.0f * lxy * lxy);
                float a = 0.5f * (tr + d), b = 0.5f * (tr - d);
                if (std::fabs(a) > std::fabs(b)) std::swap(a, b); // |a| <= |b|
                const size_t i = static_cast<size_t>(y) * w + x;
                l1[i] = a;
                l2[i] = b;
                // CRISTA, NAO DEGRAU (condicao de crista de Lindeberg 1998: no
                // centro da linha a derivada ATRAVES dela, L_p, e' zero). O
                // detector de Hessiana tambem acende dos dois lados de um
                // degrau (borda tabua/tijolo, moldura da janela) - e esse
                // degrau dominava a escala e apagava as juntas fracas. Mede
                // L_p na direcao da curvatura principal e derruba onde ela e'
                // grande comparada a curvatura.
                float ex, ey;
                if (std::fabs(lxy) > 1e-9f) { ex = b - lyy; ey = lxy; }
                else if (std::fabs(lxx) >= std::fabs(lyy)) { ex = 1.0f; ey = 0.0f; }
                else { ex = 0.0f; ey = 1.0f; }
                const float el = std::sqrt(ex * ex + ey * ey);
                ex /= el; ey /= el;
                const float gxs = 0.5f * (at(x + 1, y) - at(x - 1, y)) * sg;
                const float gys = 0.5f * (at(x, y + 1) - at(x, y - 1)) * sg;
                const float lp = std::fabs(gxs * ex + gys * ey);
                const float r = lp / (std::fabs(b) + 1e-6f);
                edgeSup[i] = std::exp(-r * r / (2.0f * 0.5f * 0.5f));
                sMax = std::max(sMax, std::sqrt(a * a + b * b));
            }
        }
        const float c = 0.5f * sMax; // "metade da maior norma de Hessiana" (Frangi)
        for (size_t i = 0; i < n; ++i) {
            const float b = l2[i];
            if (std::fabs(b) < 1e-8f) continue;
            const float rb = l1[i] / b;
            const float S2 = l1[i] * l1[i] + b * b;
            const float v = std::exp(-rb * rb / (2.0f * beta * beta)) * (1.0f - std::exp(-S2 / (2.0f * c * c))) * edgeSup[i];
            if (b > 0.0f) valley[i] = std::max(valley[i], v);
            else          ridge[i]  = std::max(ridge[i], v);
        }
    }
}

// Transformada de distancia EXATA (distancia euclidiana ao quadrado) -
// Felzenszwalb & Huttenlocher 2012, "Distance transforms of sampled
// functions", Theory of Computing 8. Envelope inferior de parabolas em 1D,
// linhas e depois colunas. Periodica: cada linha/coluna vai triplicada e so'
// o meio volta, entao a distancia atravessa a borda do ladrilho certo.
void edt1d(const std::vector<double>& f, std::vector<double>& d) {
    const int m = static_cast<int>(f.size());
    std::vector<int> v(m);
    std::vector<double> z(m + 1);
    int k = 0;
    v[0] = 0; z[0] = -1e30; z[1] = 1e30;
    for (int q = 1; q < m; ++q) {
        double s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        while (s <= z[k]) {
            --k;
            s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        }
        ++k;
        v[k] = q; z[k] = s; z[k + 1] = 1e30;
    }
    k = 0;
    d.resize(m);
    for (int q = 0; q < m; ++q) {
        while (z[k + 1] < q) ++k;
        d[q] = double(q - v[k]) * (q - v[k]) + f[v[k]];
    }
}

Img distanceToLinesWrap(const std::vector<uint8_t>& line, int w, int h) {
    const double INF = 1e20;
    std::vector<double> grid(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < grid.size(); ++i) grid[i] = line[i] ? 0.0 : INF;
    std::vector<double> f, d;
    for (int y = 0; y < h; ++y) {
        f.assign(static_cast<size_t>(3 * w), INF);
        for (int x = 0; x < 3 * w; ++x) f[x] = grid[static_cast<size_t>(y) * w + (x % w)];
        edt1d(f, d);
        for (int x = 0; x < w; ++x) grid[static_cast<size_t>(y) * w + x] = d[w + x];
    }
    for (int x = 0; x < w; ++x) {
        f.assign(static_cast<size_t>(3 * h), INF);
        for (int y = 0; y < 3 * h; ++y) f[y] = grid[static_cast<size_t>(y % h) * w + x];
        edt1d(f, d);
        for (int y = 0; y < h; ++y) grid[static_cast<size_t>(y) * w + x] = d[h + y];
    }
    Img out(grid.size());
    for (size_t i = 0; i < grid.size(); ++i) out[i] = static_cast<float>(std::sqrt(std::min(grid[i], 1e12)));
    return out;
}

// FECHAMENTO: rejunte ISOLA regioes (cada pedra/tijolo/tabua e' uma ilha
// entre linhas); rabisco de grama nao isola nada - o complemento das linhas
// continua sendo um pedaco so'. Mede a fracao dos pixels fora-de-linha que
// caem em CELULAS de tamanho de pedra/tijolo (entre minCell e 1/4 do
// ladrilho): ~0,5+ em pedra, ~0 em grama. Uma area lisa grande (reboco,
// janela) nao penaliza - so' nao conta. A mascara e' dilatada antes, pra'
// fechar as brechas da deteccao (a linha sai com furo onde o rejunte clareia
// - sem isso uma brecha liga todas as pedras).
float closureScore(const std::vector<uint8_t>& lineIn, int w, int h, int dilate,
                   std::vector<uint8_t>* cellMask = nullptr,
                   std::vector<uint8_t>* freeMask = nullptr) {
    std::vector<uint8_t> line = lineIn;
    for (int it = 0; it < dilate; ++it) {
        std::vector<uint8_t> nx = line;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (line[i]) continue;
                if (line[static_cast<size_t>(y) * w + wrapi(x + 1, w)] || line[static_cast<size_t>(y) * w + wrapi(x - 1, w)] ||
                    line[static_cast<size_t>(wrapi(y + 1, h)) * w + x] || line[static_cast<size_t>(wrapi(y - 1, h)) * w + x])
                    nx[i] = 1;
            }
        line.swap(nx);
    }
    const size_t minCell = 8, maxCell = line.size() / 4;
    std::vector<uint8_t> seen(line.size(), 0);
    std::vector<size_t> stack, comp;
    if (cellMask) cellMask->assign(line.size(), 0);
    if (freeMask) { freeMask->assign(line.size(), 0); for (size_t i = 0; i < line.size(); ++i) (*freeMask)[i] = line[i] ? 0 : 1; }
    size_t free = 0, inCells = 0;
    for (size_t s = 0; s < line.size(); ++s) {
        if (line[s] || seen[s]) continue;
        size_t count = 0;
        stack.clear();
        comp.clear();
        stack.push_back(s);
        seen[s] = 1;
        while (!stack.empty()) {
            const size_t i = stack.back(); stack.pop_back();
            comp.push_back(i);
            ++count;
            const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
            const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& o : nb) {
                const size_t j = static_cast<size_t>(wrapi(y + o[1], h)) * w + wrapi(x + o[0], w);
                if (!line[j] && !seen[j]) { seen[j] = 1; stack.push_back(j); }
            }
        }
        free += count;
        if (count >= minCell && count <= maxCell) {
            inCells += count;
            if (cellMask) for (size_t i : comp) (*cellMask)[i] = 1;
        }
    }
    return free ? static_cast<float>(inCells) / static_cast<float>(free) : 0.0f;
}

} // namespace

HeightFromAlbedoResult heightFromAlbedo(const uint8_t* rgba, int w, int h,
                                        const HeightFromAlbedoParams& prm) {
    HeightFromAlbedoResult out;
    if (!rgba || w < 4 || h < 4) return out;
    out.width = w;
    out.height = h;
    const size_t n = static_cast<size_t>(w) * h;

    // 1. Luminancia linear + confianca (preto puro = buraco pintado, nao superficie).
    Img L(n), conf(n);
    for (size_t i = 0; i < n; ++i) {
        const float r = srgbToLinear(rgba[i * 4 + 0] / 255.0f);
        const float g = srgbToLinear(rgba[i * 4 + 1] / 255.0f);
        const float b = srgbToLinear(rgba[i * 4 + 2] / 255.0f);
        L[i] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        const float t = std::clamp((L[i] - 0.004f) / (0.02f - 0.004f), 0.0f, 1.0f);
        conf[i] = t * t * (3.0f - 2.0f * t);
    }
    conf = gaussWrap(conf, w, h, 2.0f);

    // 2. Delight: tira a banda baixa (sombra pintada grande), mantem a media.
    // Antes disso um blur minimo (0.8 texel): bitmap antigo de 256 px tem
    // dithering de paleta pixel a pixel, e a banda mais fina o lia como grao
    // (aparecia como pontilhado nas pedras do chao).
    L = gaussWrap(L, w, h, 0.8f);
    const float bigSigma = static_cast<float>(std::min(w, h)) / 8.0f;
    {
        Img low = gaussWrap(L, w, h, bigSigma);
        const float m = meanOf(L);
        for (size_t i = 0; i < n; ++i) L[i] = L[i] - low[i] + m;
    }

    // 3. Detalhe multi-escala: DoG por banda, normalizada pelo RMS local.
    Img detail(n, 0.0f);
    {
        const float sigmas[4] = {1.0f, 2.0f, 4.0f, 8.0f};
        const float gains[4] = {0.06f, 0.34f, 0.42f, 0.18f};
        Img prev = L;
        for (int b = 0; b < 4; ++b) {
            Img cur = gaussWrap(L, w, h, sigmas[b] * 2.0f);
            Img band(n);
            for (size_t i = 0; i < n; ++i) band[i] = prev[i] - cur[i];
            Img sq(n);
            for (size_t i = 0; i < n; ++i) sq[i] = band[i] * band[i];
            Img energy = gaussWrap(sq, w, h, sigmas[b] * 3.0f);
            for (size_t i = 0; i < n; ++i)
                detail[i] += gains[b] * band[i] / (std::sqrt(std::max(energy[i], 0.0f)) + 0.02f);
            prev = std::move(cur);
        }
    }

    // 4. FORMA por gradiente: Scharr -> edicao do campo -> Frankot-Chellappa.
    Img shape(n, 0.0f);
    {
        const int fw = std::min(nextPow2(w), 512), fh = std::min(nextPow2(h), 512);
        Img Ls = resampleWrap(gaussWrap(L, w, h, 1.5f), w, h, fw, fh);
        Img gx, gy;
        scharr(Ls, fw, fh, gx, gy);
        // So' borda forte conta, e nenhuma domina: peso suave em torno do RMS
        // e teto em 3x RMS. E' isto que separa "junta de pedra" de "grao".
        Img mag(gx.size());
        for (size_t i = 0; i < gx.size(); ++i) mag[i] = std::sqrt(gx[i] * gx[i] + gy[i] * gy[i]);
        const float rms = std::max(rmsOf(mag), 1e-5f);
        const float t0 = 0.5f * rms, t1 = 2.0f * rms, gmax = 3.0f * rms;
        for (size_t i = 0; i < gx.size(); ++i) {
            const float t = std::clamp((mag[i] - t0) / (t1 - t0), 0.0f, 1.0f);
            const float wgt = t * t * (3.0f - 2.0f * t) * std::min(1.0f, gmax / std::max(mag[i], 1e-6f));
            gx[i] *= wgt;
            gy[i] *= wgt;
        }
        Img z = frankotChellappa(gx, gy, fw, fh);
        // Passa-alta: integracao acumula deriva de baixa frequencia.
        Img zLow = gaussWrap(z, fw, fh, static_cast<float>(std::min(fw, fh)) / 8.0f);
        for (size_t i = 0; i < z.size(); ++i) z[i] -= zLow[i];
        shape = resampleWrap(z, fw, fh, w, h);
    }

    // 5. Combina.
    //  a) Caminho ORGANICO (grama, terra - sem rede de rejunte): forma por
    //     gradiente + detalhe multi-escala, tanh.
    //  b) Caminho ESTRUTURAL (pedra, tijolo, tabua - tem rede): rejunte achado
    //     por forma (frangiLines), distancia exata ate' ele, e cada regiao
    //     "infla" com perfil de quarto de circulo a partir da borda - a ideia
    //     de inflar regioes delimitadas por linha de Johnston 2002 (Lumo) e
    //     Sykora et al. 2010 ("Adding depth to cartoons"), feita com a
    //     distancia em vez de resolver Poisson. Rejunte no fundo, borda nitida,
    //     topo da pedra alto - sem depender da cor de cada pedra.
    //  O peso entre (a) e (b) e' o quanto a textura TEM rede (networkScore).
    {
        const float sRms = std::max(rmsOf(shape), 1e-6f);
        const float dRms = std::max(rmsOf(detail), 1e-6f);
        Img organic(n);
        for (size_t i = 0; i < n; ++i) {
            const float v = prm.shapeWeight * shape[i] / sRms + prm.detailWeight * detail[i] / dRms;
            organic[i] = 0.5f + std::tanh(v * prm.outputGain) * 0.5f;
        }

        Img valley, ridge;
        frangiLines(L, w, h, valley, ridge);
        // Cada polaridade normalizada pela propria resposta alta (p98).
        auto p98of = [&](const Img& a) {
            std::vector<float> sorted(a.begin(), a.end());
            std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(n * 98 / 100), sorted.end());
            return std::max(sorted[n * 98 / 100], 1e-6f);
        };
        // Escala COMUM as duas polaridades: normalizar cada uma pelo proprio
        // p98 fazia o ruido fraco de crista (dentro da pedra) valer tanto
        // quanto o rejunte de verdade, e a textura escolhia a polaridade
        // errada. Na mesma escala, forte continua forte.
        const float pCommon = std::max(p98of(valley), p98of(ridge));
        for (size_t i = 0; i < n; ++i) { valley[i] /= pCommon; ridge[i] /= pCommon; }
        // POLARIDADE POR REGIAO (contexto): numa textura que mistura tabua
        // (junta escura) e tijolo (argamassa clara), cada pedaco usa a que
        // domina NA VIZINHANCA (blur de 1/8 do ladrilho), nao a da textura
        // inteira.
        // POLARIDADE: a da textura inteira (energia confiante, > 0.3) manda;
        // uma regiao so' troca pra' outra quando ela domina ALI com folga
        // (2x) - e' o caso da parede que mistura tabua (junta escura) e
        // tijolo (argamassa clara). Troca por qualquer margem deixava o ruido
        // de crista de dentro da pedra ganhar do rejunte.
        Img vW(n), rW(n);
        double eV = 0.0, eR = 0.0;
        for (size_t i = 0; i < n; ++i) {
            vW[i] = valley[i] > 0.3f ? valley[i] * conf[i] : 0.0f;
            rW[i] = ridge[i] > 0.3f ? ridge[i] * conf[i] : 0.0f;
            eV += vW[i]; eR += rW[i];
        }
        const bool globalValley = eV >= eR;
        const float regionSigma = static_cast<float>(std::min(w, h)) / 6.0f;
        vW = gaussWrap(vW, w, h, regionSigma);
        rW = gaussWrap(rW, w, h, regionSigma);
        Img resp(n);
        for (size_t i = 0; i < n; ++i) {
            const bool useV = globalValley ? !(rW[i] > 2.0f * vW[i]) : (vW[i] > 2.0f * rW[i]);
            resp[i] = (useV ? valley[i] : ridge[i]) * (conf[i] > 0.5f ? 1.0f : 0.0f);
        }
        // HISTERESE (Canny 1986, "A computational approach to edge
        // detection", IEEE PAMI): semente so' onde a resposta e' forte, e a
        // linha cresce por resposta FRACA desde que conectada a forte. A
        // escala comum e' dominada pelas bordas gigantes (moldura de janela,
        // divisa tabua/tijolo), entao a junta vertical do tijolo fica abaixo
        // do limiar - sem isto as fiadas nao fechavam celula e o tijolo caia
        // no caminho organico (cor vira altura). Resposta fraca SOLTA continua
        // de fora.
        std::vector<uint8_t> line(n, 0);
        {
            const float hi = 0.35f, lo = 0.12f;
            std::vector<size_t> stack;
            for (size_t i = 0; i < n; ++i) if (resp[i] > hi) { line[i] = 1; stack.push_back(i); }
            while (!stack.empty()) {
                const size_t i = stack.back(); stack.pop_back();
                const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const size_t j = static_cast<size_t>(wrapi(y + dy, h)) * w + wrapi(x + dx, w);
                        if (!line[j] && resp[j] > lo) { line[j] = 1; stack.push_back(j); }
                    }
            }
        }
        size_t linePx = 0;
        for (size_t i = 0; i < n; ++i) linePx += line[i];
        const float frac = static_cast<float>(linePx) / static_cast<float>(n);
        auto sstep = [](float e0, float e1, float x) {
            const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        // PESO ESTRUTURAL POR PIXEL (contexto local, nao um numero da textura
        // inteira): e' estrutura onde as linhas FECHAM celulas de tamanho de
        // pedra/tijolo. Textura mista (tabua + tijolo + reboco) ganha relevo
        // so' onde tem rede; grama (rabisco que nao fecha nada) fica organica.
        std::vector<uint8_t> cells, freeM;
        const float closure = closureScore(line, w, h, 2, &cells, &freeM);
        // Buraco preto (janela) nao e' celula: nao infla, fica neutro.
        for (size_t i = 0; i < n; ++i) if (conf[i] < 0.5f) cells[i] = 0;
        // Evidencia local = que fracao do espaco LIVRE (fora das linhas
        // dilatadas) da vizinhanca esta' dentro de celula fechada.
        Img cellsBlur(n), freeBlur(n);
        for (size_t i = 0; i < n; ++i) { cellsBlur[i] = cells[i] ? 1.0f : 0.0f; freeBlur[i] = freeM[i] ? 1.0f : 0.0f; }
        cellsBlur = gaussWrap(cellsBlur, w, h, 6.0f);
        freeBlur = gaussWrap(freeBlur, w, h, 6.0f);
        Img structWpx(n);
        const float presence = prm.forceOrganic ? 0.0f : sstep(0.03f, 0.08f, frac);
        double wSum = 0.0;
        // Segunda evidencia: DENSIDADE de linha na vizinhanca. Parede de
        // tijolo tem as fiadas (horizontais) fortes e as juntas verticais
        // fracas - as celulas nao fecham, mas as linhas paralelas densas ja'
        // dizem "alvenaria". Grama tambem tem densidade alta, por isso ela
        // e' excluida antes, pela categoria do material (forceOrganic).
        Img lineBlur(n);
        for (size_t i = 0; i < n; ++i) lineBlur[i] = line[i] ? 1.0f : 0.0f;
        lineBlur = gaussWrap(lineBlur, w, h, 6.0f);
        for (size_t i = 0; i < n; ++i) {
            const float evCells = sstep(0.35f, 0.7f, cellsBlur[i] / std::max(freeBlur[i], 0.05f));
            const float evDensity = sstep(0.10f, 0.20f, lineBlur[i]);
            structWpx[i] = presence * std::max(evCells, evDensity) * (conf[i] > 0.5f ? 1.0f : 0.0f);
            wSum += structWpx[i];
        }
        const float structW = static_cast<float>(wSum / static_cast<double>(n));
        // ERUPTION_TEST_HFA_DUMP=<prefixo>: grava as mascaras intermediarias
        // (PGM 8 bits) - vale, crista, linhas, celulas, peso estrutural.
        if (const char* dp = std::getenv("ERUPTION_TEST_HFA_DUMP")) {
            auto dump = [&](const char* tag, auto val) {
                std::string path = std::string(dp) + "_" + tag + ".pgm";
                if (FILE* fp = std::fopen(path.c_str(), "wb")) {
                    std::fprintf(fp, "P5\n%d %d\n255\n", w, h);
                    for (size_t i = 0; i < n; ++i) {
                        const unsigned char c = static_cast<unsigned char>(std::clamp(val(i), 0.0f, 1.0f) * 255.0f);
                        std::fputc(c, fp);
                    }
                    std::fclose(fp);
                }
            };
            dump("valley", [&](size_t i) { return valley[i]; });
            dump("ridge", [&](size_t i) { return ridge[i]; });
            dump("line", [&](size_t i) { return line[i] ? 1.0f : 0.0f; });
            dump("cells", [&](size_t i) { return cells[i] ? 1.0f : 0.0f; });
            dump("structw", [&](size_t i) { return structWpx[i]; });
        }
        out.structureWeight = structW;
        if (std::getenv("ERUPTION_TEST_HFA_STATS")) {
            std::fprintf(stderr, "[HFA] %dx%d linhas=%.3f fechamento=%.3f estrutura_media=%.3f organico=%d\n",
                         w, h, frac, closure, structW, prm.forceOrganic ? 1 : 0);
        }

        Img structural(n, 0.5f);
        if (structW > 0.0f) {
            Img dist = distanceToLinesWrap(line, w, h);
            double dSum = 0.0; size_t dCnt = 0;
            // Media so' DENTRO da regiao estrutural: a area sem rejunte (tabua,
            // reboco) tem distancia enorme e inflava o raio - tijolo pequeno
            // nunca chegava ao topo do chanfro e saia cinza.
            for (size_t i = 0; i < n; ++i)
                if (!line[i] && conf[i] > 0.5f && structWpx[i] > 0.5f) { dSum += dist[i]; ++dCnt; }
            const float meanD = dCnt ? static_cast<float>(dSum / static_cast<double>(dCnt)) : 4.0f;
            const float R = std::clamp(1.6f * meanD, 2.0f, 24.0f); // raio do chanfro, em texel
            for (size_t i = 0; i < n; ++i) {
                const float t = std::min(dist[i] / R, 1.0f);
                const float cushion = std::sqrt(t * (2.0f - t));   // quarto de circulo
                const float grain = 0.10f * std::tanh(detail[i] / dRms); // textura da face, leve
                structural[i] = std::clamp(0.06f + 0.86f * cushion + grain, 0.0f, 1.0f);
            }
        }

        out.heightMap.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const float hv = organic[i] + (structural[i] - organic[i]) * structWpx[i];
            out.heightMap[i] = 0.5f + (hv - 0.5f) * conf[i];
        }
    }

    // 6. Normal HIBRIDA multi-escala a partir da ALTURA (ver .hpp):
    //    n_raw = sum_s w_s * sigma_s^gamma * grad(G_sigma_s * h) * c_s
    //    gamma = 0.6 (regime de borda, Lindeberg 1998); c_s = confianca por
    //    escala de Fattal 2002: (|g|/alpha)^(beta-1), alpha = 0.1*media|g|,
    //    beta = 0.85 - comprime o outlier e levanta a linha fraca.
    {
        const float sigmas[5] = {0.7f, 1.5f, 3.0f, 6.0f, 12.0f};
        const float fineW[5]   = {0.50f, 0.30f, 0.15f, 0.05f, 0.00f};
        const float coarseW[5] = {0.00f, 0.10f, 0.25f, 0.35f, 0.30f};
        const float t = std::clamp(prm.normalShapeMix, 0.0f, 1.0f);
        const float gamma = 0.6f, beta = 0.85f;
        Img nxAcc(n, 0.0f), nyAcc(n, 0.0f);
        for (int s = 0; s < 5; ++s) {
            const float ws = (1.0f - t) * fineW[s] + t * coarseW[s];
            if (ws <= 0.0f) continue;
            Img hs = gaussWrap(out.heightMap, w, h, sigmas[s]);
            Img gx, gy;
            scharr(hs, w, h, gx, gy);
            double magSum = 0.0;
            for (size_t i = 0; i < n; ++i) magSum += std::sqrt(gx[i] * gx[i] + gy[i] * gy[i]);
            const float alpha = std::max(0.1f * static_cast<float>(magSum / static_cast<double>(n)), 1e-6f);
            const float scaleW = ws * std::pow(sigmas[s], gamma);
            for (size_t i = 0; i < n; ++i) {
                const float mag = std::sqrt(gx[i] * gx[i] + gy[i] * gy[i]);
                const float c = mag > 1e-8f ? std::pow(mag / alpha, beta - 1.0f) : 1.0f;
                nxAcc[i] += scaleW * c * gx[i];
                nyAcc[i] += scaleW * c * gy[i];
            }
        }
        // Escala global: RMS unitario da inclinacao antes do strength, pra' o
        // slider significar o mesmo em qualquer textura.
        double ss = 0.0;
        for (size_t i = 0; i < n; ++i) ss += static_cast<double>(nxAcc[i]) * nxAcc[i] + static_cast<double>(nyAcc[i]) * nyAcc[i];
        const float rmsSlope = std::max(static_cast<float>(std::sqrt(ss / static_cast<double>(n))), 1e-6f);
        const float k = prm.normalStrength * 0.15f / rmsSlope;
        out.normalRgba.resize(n * 4);
        for (size_t i = 0; i < n; ++i) {
            // +Y pra cima (OpenGL, a convencao que o motor le'): nyAcc e'
            // dh/dlinha com linha crescendo pra baixo, entao ny = +nyAcc.
            // Com -nyAcc o relevo saia invertido no eixo vertical (medido no
            // banco: g do normal -0,08 contra o gabarito).
            float nx = -nxAcc[i] * k;
            float ny = nyAcc[i] * k;
            float nz = 1.0f;
            const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            nx *= inv; ny *= inv; nz *= inv;
            out.normalRgba[i * 4 + 0] = static_cast<uint8_t>(std::clamp((nx * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
            out.normalRgba[i * 4 + 1] = static_cast<uint8_t>(std::clamp((ny * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
            out.normalRgba[i * 4 + 2] = static_cast<uint8_t>(std::clamp((nz * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
            out.normalRgba[i * 4 + 3] = 255;
        }
    }

    // 7. Cavidade: so' o lado fundo do contraste local de h.
    {
        Img hb = gaussWrap(out.heightMap, w, h, 3.0f);
        out.cavity.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const float rel = std::tanh((out.heightMap[i] - hb[i]) * 4.0f);
            out.cavity[i] = std::clamp(1.0f + std::min(rel, 0.0f) * 1.2f, 0.35f, 1.0f);
        }
    }
    return out;
}

} // namespace eruption
