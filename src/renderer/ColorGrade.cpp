#include "renderer/ColorGrade.hpp"
#include "utils/AssetVfs.hpp"
#include "renderer/LookConfig.hpp"
#include "utils/SplineCurve.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stb_image.h>

namespace eruption {

namespace {

const Vec3 kLuma(0.2126f, 0.7152f, 0.0722f);

// Rec.709 linear <-> XYZ (D65).
const Mat3 kRgbToXyz = glm::transpose(Mat3(0.4124564f, 0.3575761f, 0.1804375f,
                                           0.2126729f, 0.7151522f, 0.0721750f,
                                           0.0193339f, 0.1191920f, 0.9503041f));
// Bradford (adaptacao cromatica): XYZ -> "cone" LMS achatado.
const Mat3 kBradford = glm::transpose(Mat3(0.8951f, 0.2664f, -0.1614f,
                                           -0.7502f, 1.7135f, 0.0367f,
                                           0.0389f, -0.0685f, 1.0296f));

Vec3 xyToXyz(float x, float y) { return Vec3(x / y, 1.0f, (1.0f - x - y) / y); }

// BALANCO DE BRANCO: temperatura e matiz (-100..100, 0 = neutro) movem o
// branco de referencia ao longo do locus do iluminante D e para o verde/
// magenta; a cor e' adaptada do branco escolhido para o D65 por Bradford
// (a conta do "white balance" dos editores de foto). Temperatura > 0 esquenta
// (a cena e' tratada como filmada sob luz mais fria e compensada); matiz > 0
// puxa para o magenta.
Mat3 whiteBalanceMatrix(float temperature, float tint) {
    if (temperature == 0.0f && tint == 0.0f) return Mat3(1.0f);
    // t1 > 0 tira o branco de referencia para o azul: adaptar dele para o
    // D65 puxa a cena para o quente.
    const float t1 = temperature / 65.0f, t2 = tint / 65.0f;
    const float x = 0.31271f - t1 * (t1 < 0.0f ? 0.1f : 0.05f);
    // y do locus do iluminante D em funcao de x (CIE).
    const float yLocus = 2.87f * x - 3.0f * x * x - 0.27509507f;
    const float y = yLocus + t2 * 0.05f;
    const Vec3 lmsSrc = kBradford * xyToXyz(x, y);
    const Vec3 lmsDst = kBradford * xyToXyz(0.31271f, 0.32902f);
    const Mat3 scale(Vec3(lmsDst.x / lmsSrc.x, 0, 0), Vec3(0, lmsDst.y / lmsSrc.y, 0), Vec3(0, 0, lmsDst.z / lmsSrc.z));
    const Mat3 cat = glm::inverse(kBradford) * scale * kBradford;
    return glm::inverse(kRgbToXyz) * cat * kRgbToXyz;
}

float smoothstep01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// Mesma conta do applyGrading do post_composite.frag.
Vec3 analyticGrading(Vec3 c, float saturation, float contrast) {
    const float luma = glm::dot(c, kLuma);
    c = glm::mix(Vec3(luma), c, saturation);
    constexpr float kPivot = 0.18f;
    c = glm::max(c, Vec3(0.0f));
    return kPivot * glm::pow(c / kPivot, Vec3(contrast));
}

// Mesma conta do applySplitToning do post_composite.frag.
Vec3 splitToning(Vec3 c, const Vec4& shadowTint, const Vec4& highlightTint) {
    if (shadowTint.a <= 0.0f && highlightTint.a <= 0.0f) return c;
    const float l = glm::dot(c, kLuma);
    const float t = smoothstep01(l);
    const Vec3 sh = Vec3(shadowTint) / std::max(glm::dot(Vec3(shadowTint), kLuma), 1e-3f);
    const Vec3 hi = Vec3(highlightTint) / std::max(glm::dot(Vec3(highlightTint), kLuma), 1e-3f);
    Vec3 r = c * glm::mix(Vec3(1.0f), sh, std::clamp(shadowTint.a, 0.0f, 1.0f) * (1.0f - t));
    r *= glm::mix(Vec3(1.0f), hi, std::clamp(highlightTint.a, 0.0f, 1.0f) * t);
    return r * (l / std::max(glm::dot(r, kLuma), 1e-5f));
}

float encode(float v) { return std::pow(std::max(v, 0.0f), 1.0f / 2.2f); }
float decode(float v) { return std::pow(std::max(v, 0.0f), 2.2f); }

} // namespace

bool CubeLut::loadStrip(const std::string& path) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, STBI_rgb);
    if (!px) { // fora do disco: pacote .ecp do projeto
        std::vector<uint8_t> bytes;
        if (vfs::read(path, bytes) && !bytes.empty())
            px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, STBI_rgb);
    }
    if (!px) return false;
    const bool ok = h >= 2 && h <= 128 && w == h * h;
    if (ok) {
        size = h;
        data.resize(static_cast<size_t>(size) * size * size);
        for (int b = 0; b < size; ++b)
            for (int g = 0; g < size; ++g)
                for (int r = 0; r < size; ++r) {
                    const unsigned char* p = px + (static_cast<size_t>(g) * w + b * size + r) * 3;
                    data[(static_cast<size_t>(b) * size + g) * size + r] = Vec3(p[0], p[1], p[2]) / 255.0f;
                }
    }
    stbi_image_free(px);
    return ok;
}

bool CubeLut::load(const std::string& path) {
    const std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : std::string();
    if (ext == ".png" || ext == ".PNG") return loadStrip(path);
    std::string text; // disco ou pacote .ecp
    if (!vfs::readText(path, text)) return false;
    std::istringstream f(text);
    size = 0;
    data.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        std::string first;
        in >> first;
        if (first == "LUT_3D_SIZE") {
            in >> size;
            if (size < 2 || size > 128) return false;
            data.reserve(static_cast<size_t>(size) * size * size);
        } else if (first == "TITLE" || first == "DOMAIN_MIN" || first == "DOMAIN_MAX" || first == "LUT_1D_SIZE") {
            if (first == "LUT_1D_SIZE") return false; // so' LUT 3D
        } else {
            Vec3 v;
            v.x = std::strtof(first.c_str(), nullptr);
            in >> v.y >> v.z;
            if (!in.fail()) data.push_back(v);
        }
    }
    const bool ok = size > 0 && data.size() == static_cast<size_t>(size) * size * size;
    if (!ok) { size = 0; data.clear(); }
    return ok;
}

Vec3 CubeLut::sample(const Vec3& c) const {
    if (size < 2) return c;
    const Vec3 p = glm::clamp(c, Vec3(0.0f), Vec3(1.0f)) * float(size - 1);
    const glm::ivec3 i0 = glm::min(glm::ivec3(p), glm::ivec3(size - 2));
    const Vec3 f = p - Vec3(i0);
    auto at = [&](int r, int g, int b) { return data[(static_cast<size_t>(b) * size + g) * size + r]; };
    Vec3 acc(0.0f);
    for (int k = 0; k < 8; ++k) {
        const int dr = k & 1, dg = (k >> 1) & 1, db = (k >> 2) & 1;
        const float w = (dr ? f.x : 1.0f - f.x) * (dg ? f.y : 1.0f - f.y) * (db ? f.z : 1.0f - f.z);
        acc += w * at(i0.x + dr, i0.y + dg, i0.z + db);
    }
    return acc;
}

Vec3 gradeColor(const LookConfig& look, Vec3 c, const CubeLut* cube) {
    c = glm::max(c, Vec3(0.0f));
    // 1. Balanco de branco (linear).
    c = glm::max(whiteBalanceMatrix(look.wbTemperature, look.wbTint) * c, Vec3(0.0f));
    // 2. Lift / gamma / gain por canal (sobre a cor de tela 0..1).
    if (look.lift != Vec3(0.0f) || look.gain != Vec3(1.0f) || look.gammaRgb != Vec3(1.0f)) {
        c = look.gain * (c + look.lift * (Vec3(1.0f) - glm::min(c, Vec3(1.0f))));
        c = glm::pow(glm::max(c, Vec3(0.0f)), Vec3(1.0f) / glm::max(look.gammaRgb, Vec3(0.05f)));
    }
    // 3. Saturacao e contraste (o grading de sempre).
    c = analyticGrading(c, look.saturation, look.contrast);
    // 4. Vibrance: satura mais o que tem pouca cor (pele e ceu ja' saturados
    // quase nao mudam).
    if (look.vibrance != 0.0f) {
        const float mx = std::max(c.r, std::max(c.g, c.b)), mn = std::min(c.r, std::min(c.g, c.b));
        const float sat = mx > 1e-5f ? (mx - mn) / mx : 0.0f;
        const float luma = glm::dot(c, kLuma);
        c = glm::max(glm::mix(Vec3(luma), c, 1.0f + look.vibrance * (1.0f - sat)), Vec3(0.0f));
    }
    // 5. Split-toning.
    c = splitToning(c, Vec4(look.shadowTint, look.shadowTintStrength), Vec4(look.highlightTint, look.highlightTintStrength));
    // 6. Curva mestra (pares x,y em 0..1, em gamma de tela), por canal.
    if (look.curvePoints.size() >= 4) {
        std::vector<float> xs, ys;
        for (size_t i = 0; i + 1 < look.curvePoints.size(); i += 2) {
            xs.push_back(look.curvePoints[i]);
            ys.push_back(look.curvePoints[i + 1]);
        }
        const SplineCurve curve(xs, ys, InterpolationMode::Smoothstep);
        for (int k = 0; k < 3; ++k) c[k] = decode(curve.evaluate(encode(std::min(c[k], 1.0f)))) + std::max(c[k] - 1.0f, 0.0f);
    }
    // 7. LUT .cube do colorista, em gamma de tela.
    if (cube && cube->size > 1 && look.lutStrength > 0.0f) {
        const Vec3 e(encode(c.r), encode(c.g), encode(c.b));
        const Vec3 g = cube->sample(e);
        const Vec3 lin(decode(g.r), decode(g.g), decode(g.b));
        c = glm::mix(c, lin, std::clamp(look.lutStrength, 0.0f, 1.0f));
    }
    return c;
}

std::vector<uint32_t> bakeGradeLut(const LookConfig& look, const CubeLut* cube, int n) {
    std::vector<uint32_t> out(static_cast<size_t>(n) * n * n);
    auto pack = [](float v) {
        return static_cast<uint32_t>(std::lround(std::clamp(encode(v / kGradeLutRange), 0.0f, 1.0f) * 1023.0f));
    };
    for (int b = 0; b < n; ++b) {
        for (int g = 0; g < n; ++g) {
            for (int r = 0; r < n; ++r) {
                const Vec3 in(decode(float(r) / float(n - 1)), decode(float(g) / float(n - 1)),
                              decode(float(b) / float(n - 1)));
                const Vec3 o = gradeColor(look, in, cube);
                out[(static_cast<size_t>(b) * n + g) * n + r] = pack(o.r) | (pack(o.g) << 10) | (pack(o.b) << 20) | (3u << 30);
            }
        }
    }
    return out;
}

bool lookWantsGradeLut(const LookConfig& look) {
    return look.colorLut || look.wbTemperature != 0.0f || look.wbTint != 0.0f || look.lift != Vec3(0.0f) ||
           look.gammaRgb != Vec3(1.0f) || look.gain != Vec3(1.0f) || look.vibrance != 0.0f ||
           look.curvePoints.size() >= 4 || (!look.lutPath.empty() && look.lutStrength > 0.0f);
}

uint64_t gradeLutHash(const LookConfig& look) {
    // FNV-1a sobre os campos que entram na LUT.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    const float f[] = {look.saturation, look.contrast, look.wbTemperature, look.wbTint, look.vibrance,
                       look.lutStrength, look.shadowTintStrength, look.highlightTintStrength};
    mix(f, sizeof(f));
    const Vec3 v[] = {look.lift, look.gammaRgb, look.gain, look.shadowTint, look.highlightTint};
    mix(v, sizeof(v));
    if (!look.curvePoints.empty()) mix(look.curvePoints.data(), look.curvePoints.size() * sizeof(float));
    mix(look.lutPath.data(), look.lutPath.size());
    return h;
}

} // namespace eruption
