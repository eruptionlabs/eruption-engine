#include "utils/PbrMapGen.hpp"

#include "utils/NormalMapGen.hpp"
#include "utils/HeightFromAlbedo.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace eruption {

namespace {

inline float byteToFloat(uint8_t b) { return static_cast<float>(b) / 255.0f; }
inline uint8_t floatToByte(float v) {
    v = std::max(0.0f, std::min(1.0f, v));
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

float rgbToLuma(uint8_t r, uint8_t g, uint8_t b) {
    return byteToFloat(r) * 0.2126f + byteToFloat(g) * 0.7152f + byteToFloat(b) * 0.0722f;
}

// Simple box blur used for the roughness variance estimator.
std::vector<float> boxBlur(const std::vector<float>& src, int w, int h, int radius) {
    std::vector<float> tmp(w * h);
    std::vector<float> dst(w * h);

    // Horizontal pass
    for (int y = 0; y < h; ++y) {
        float sum = 0.0f;
        for (int x = -radius; x <= radius; ++x) {
            int ix = std::clamp(x, 0, w - 1);
            sum += src[y * w + ix];
        }
        for (int x = 0; x < w; ++x) {
            int left = std::clamp(x - radius - 1, 0, w - 1);
            int right = std::clamp(x + radius, 0, w - 1);
            sum += src[y * w + right] - src[y * w + left];
            tmp[y * w + x] = sum / static_cast<float>(2 * radius + 1);
        }
    }

    // Vertical pass
    for (int x = 0; x < w; ++x) {
        float sum = 0.0f;
        for (int y = -radius; y <= radius; ++y) {
            // BUG consertado: usava tmp[y*w+x] com y NEGATIVO (leitura fora do
            // buffer) - o iy clampado era calculado e ignorado. E os clamps de
            // linha abaixo usavam w-1 (largura) em vez de h-1 (altura).
            int iy = std::clamp(y, 0, h - 1);
            sum += tmp[iy * w + x];
        }
        for (int y = 0; y < h; ++y) {
            int top = std::clamp(y - radius - 1, 0, h - 1);
            int bottom = std::clamp(y + radius, 0, h - 1);
            sum += tmp[bottom * w + x] - tmp[top * w + x];
            dst[y * w + x] = sum / static_cast<float>(2 * radius + 1);
        }
    }
    return dst;
}

} // namespace

// The cooked maps are intentionally neutral ("100%"): roughness and metallic
// are baked at full strength so the engine can scale them down at runtime with
// the per-material profile table (see data/pbr_materials.json). Only spatial
// detail (variance, curvature, height) is baked in, since that is a property
// of the image itself and not of the material category.
PbrMapSet generatePbrMaps(const uint8_t* rgba, int width, int height,
                          const std::string& nameHint,
                          float normalStrength) {
    (void)nameHint; // material values come from the runtime profile table
    PbrMapSet out;
    if (!rgba || width <= 0 || height <= 0) return out;

    const int count = width * height;
    out.width = width;
    out.height = height;
    out.mrahw.resize(count * 4);
    out.normal.resize(count * 4);

    // Stage 1/2: altura + normal + cavidade (HeightFromAlbedo.hpp - mesma
    // conta do bake de load, synthesizePbrFromPixels). O que estava aqui era
    // heightF = luma (claro = alto, pixel a pixel) e Sobel na luminancia.
    HeightFromAlbedoParams hp;
    hp.normalStrength = 3.0f * normalStrength;
    const HeightFromAlbedoResult hfa = heightFromAlbedo(rgba, width, height, hp);
    if (hfa.heightMap.empty()) return out;
    out.normal = hfa.normalRgba;

    std::vector<float> luma(count);
    for (int i = 0; i < count; ++i) {
        luma[i] = rgbToLuma(rgba[i * 4 + 0], rgba[i * 4 + 1], rgba[i * 4 + 2]);
    }

    // Stage 3: Roughness baked at 100% with a small variance-based detail
    // reduction on high-contrast edges. The engine multiplies this by the
    // material profile roughness at runtime.
    std::vector<float> blurred = boxBlur(luma, width, height, 2); // 5x5 approx with radius=2
    for (int i = 0; i < count; ++i) {
        float diff = std::abs(luma[i] - blurred[i]);
        float variance = std::pow(1.0f - diff, 1.5f); // 1 on flat areas, lower on edges
        float roughness = 1.0f - 0.15f * (1.0f - variance);
        out.mrahw[i * 4 + 1] = floatToByte(roughness);
    }

    // Stage 4: Metallic baked at 100%; the engine scales it by the material
    // profile metallic factor at runtime.
    for (int i = 0; i < count; ++i) {
        out.mrahw[i * 4 + 0] = 255;
    }

    // Stage 5/6: AO em .b e ALTURA em .a - o layout que os DOIS geradores de
    // runtime (loadIndustryStandardPbr, synthesizePbrFromPixels) e os shaders
    // usam (model.frag/terrain.frag: `aoTerm *= mix(1, mrahw.b, 0.5)`;
    // `hProbe = .a`, 255 = "sem altura" -> proxy de luminancia).
    //
    // ANTES (ate 2026-09-05) este cooker escrevia altura em .b e uma mascara
    // de wetness em .a: o shader lia a ALTURA como AO (escurecia 50% toda
    // area baixa) e a mascara de wetness (~0) como altura -> useDisp ligava
    // com h=0 e a micro-sombra do sol ficava MORTA em 97% do acervo (3.568
    // dos 3.694 _mrahw cozidos). Qualquer medicao de AO anterior a isso
    // mediu altura. A mascara de wetness nao tinha leitor com esse sentido
    // (o clima entra por UBO desde IGNIS G3), entao foi removida.
    //
    // AO = cavidade por CONTRASTE LOCAL da altura (mesma familia da sintese
    // em PbrTextureLoader.cpp): altura absoluta nao e' oclusao - uma tabua
    // escura viraria buraco. (h - blur(h)) isola o sulco; tanh aprofunda sem
    // estourar em textura ruidosa; piso 0.35 pra nao virar preto. Mantido
    // IDENTICO em tools/migrate_mrahw_layout.py, que converteu os cozidos
    // existentes sem recozer (preserva R/G e os normais bit a bit).
    for (int i = 0; i < count; ++i) {
        out.mrahw[i * 4 + 2] = floatToByte(hfa.cavity[i]);
        // 254 e' o teto: 255 = flag "sem altura" no shader.
        out.mrahw[i * 4 + 3] = static_cast<uint8_t>(std::min(254.0f, hfa.heightMap[i] * 255.0f + 0.5f));
    }

    return out;
}

} // namespace eruption
