#include "renderer/vfx/AmbientParticles.hpp"

#include "core/Logger.hpp"
#include "renderer/vfx/VfxSystem.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace eruption {

bool AmbientLayer::kindFromName(const std::string& name, Kind& out) {
    if (name == "dust") out = Kind::Dust;
    else if (name == "leaves") out = Kind::Leaves;
    else if (name == "fireflies") out = Kind::Fireflies;
    else if (name == "embers") out = Kind::Embers;
    else return false;
    return true;
}

namespace {

VfxGradient::Key key(float t, float r, float g, float b, float a) {
    VfxGradient::Key k;
    k.t = t;
    k.color = Vec4(r, g, b, a);
    return k;
}

// Um emissor por camada. `count` = particulas vivas em regime (taxa x vida
// media); a rajada inicial enche a caixa de uma vez, sem esperar uma vida.
// Valores em metros (o VfxSystem converte para unidades do mundo).
VfxEffectDesc buildEffect(const AmbientLayer& l, float& yOffset) {
    VfxEmitterDesc e;
    e.name = "ar";
    e.shape = VfxShape::Box;
    e.spread = 1.0f;
    e.blend = VfxBlend::Additive;
    e.softDistance = 0.3f;
    e.nearFade = 1.0f;
    e.maxScreenSize = 0.05f;
    float count = 0.0f, radius = 14.0f, height = 7.0f;
    switch (l.kind) {
    case AmbientLayer::Kind::Dust:
        // Poeira em suspensao: quase parada, iluminada pelo sol ATRAVES do
        // mapa de sombra (lit + shadowed) - acende no facho de luz e quase
        // some na sombra, que e' o que desenha o facho no ar.
        count = 900.0f;
        e.lifetime = Vec2(6.0f, 11.0f);
        e.speed = Vec2(0.02f, 0.12f);
        e.size = Vec2(0.04f, 0.09f);
        e.gravity = Vec3(0.0f, -0.01f, 0.0f);
        e.drag = 0.6f;
        e.noiseStrength = 0.18f;
        e.noiseFrequency = 0.35f;
        e.noiseSpeed = 0.25f;
        e.color.keys = {key(0.0f, 1.0f, 0.93f, 0.78f, 0.0f), key(0.2f, 1.0f, 0.93f, 0.78f, 0.9f),
                        key(0.8f, 1.0f, 0.93f, 0.78f, 0.9f), key(1.0f, 1.0f, 0.93f, 0.78f, 0.0f)};
        e.texture = "builtin:spark";
        e.emissive = 2.2f;
        e.lit = 1.0f;
        e.shadowed = true;
        break;
    case AmbientLayer::Kind::Leaves:
        // Folha caindo: arrasto alto (cai devagar, ~0,5 m/s) e turbulencia
        // forte (vai de um lado a outro); gira no plano da tela.
        count = 75.0f;
        radius = 13.0f;
        height = 8.0f;
        e.lifetime = Vec2(9.0f, 14.0f);
        e.speed = Vec2(0.1f, 0.4f);
        e.size = Vec2(0.14f, 0.22f);
        e.gravity = Vec3(0.0f, -0.55f, 0.0f);
        e.drag = 1.1f;
        e.noiseStrength = 1.1f;
        e.noiseFrequency = 0.3f;
        e.noiseSpeed = 0.6f;
        e.rotation = Vec2(0.0f, 360.0f);
        e.angularVelocity = Vec2(-160.0f, 160.0f);
        // Cor escura de proposito: a luz do sol (lit) multiplica por ~2-3.
        e.color.keys = {key(0.0f, 0.45f, 0.45f, 0.45f, 0.0f), key(0.08f, 0.45f, 0.45f, 0.45f, 1.0f),
                        key(0.85f, 0.45f, 0.45f, 0.45f, 1.0f), key(1.0f, 0.45f, 0.45f, 0.45f, 0.0f)};
        e.texture = "builtin:leaf";
        e.flipCols = e.flipRows = 2;
        e.flipMode = VfxFlipMode::Random;
        e.flipBlend = false;
        e.blend = VfxBlend::Premultiplied;
        e.emissive = 1.0f;
        e.lit = 1.0f;
        e.shadowed = true;
        e.maxScreenSize = 0.08f;
        break;
    case AmbientLayer::Kind::Fireflies:
        // Vaga-lume: ponto quente que vagueia baixo e pisca (o gradiente da
        // vida acende e apaga tres vezes); luz propria, brilha no bloom.
        count = 90.0f;
        height = 3.5f;
        e.lifetime = Vec2(5.0f, 9.0f);
        e.speed = Vec2(0.1f, 0.35f);
        e.size = Vec2(0.08f, 0.13f);
        e.drag = 0.5f;
        e.noiseStrength = 0.7f;
        e.noiseFrequency = 0.45f;
        e.noiseSpeed = 0.5f;
        e.color.keys = {key(0.0f, 0.75f, 1.0f, 0.3f, 0.0f),  key(0.12f, 0.75f, 1.0f, 0.3f, 1.0f),
                        key(0.25f, 0.75f, 1.0f, 0.3f, 0.1f), key(0.4f, 0.75f, 1.0f, 0.3f, 1.0f),
                        key(0.55f, 0.75f, 1.0f, 0.3f, 0.05f), key(0.72f, 0.75f, 1.0f, 0.3f, 1.0f),
                        key(0.88f, 0.75f, 1.0f, 0.3f, 0.2f), key(1.0f, 0.75f, 1.0f, 0.3f, 0.0f)};
        e.texture = "builtin:spark";
        e.emissive = 4.0f;
        break;
    case AmbientLayer::Kind::Embers:
        // Brasa: sobe (empuxo), esfria do amarelo ao vermelho e apaga.
        count = 140.0f;
        height = 5.0f;
        e.lifetime = Vec2(3.0f, 6.0f);
        e.speed = Vec2(0.3f, 0.9f);
        e.direction = Vec3(0.0f, 1.0f, 0.0f);
        e.spread = 0.45f;
        e.size = Vec2(0.03f, 0.06f);
        e.gravity = Vec3(0.0f, 0.18f, 0.0f);
        e.drag = 0.4f;
        e.noiseStrength = 0.8f;
        e.noiseFrequency = 0.6f;
        e.noiseSpeed = 0.9f;
        e.stretch = 0.25f;
        e.color.keys = {key(0.0f, 1.0f, 0.75f, 0.3f, 0.0f), key(0.1f, 1.0f, 0.62f, 0.2f, 1.0f),
                        key(0.65f, 1.0f, 0.32f, 0.06f, 0.85f), key(1.0f, 0.5f, 0.08f, 0.0f, 0.0f)};
        e.texture = "builtin:spark";
        e.emissive = 3.0f;
        break;
    }
    // Caixa maior que a do tipo: mais particulas, a mesma densidade no ar.
    const float area0 = radius * radius * height;
    if (l.radius > 0.0f) radius = l.radius;
    if (l.height > 0.0f) height = l.height;
    count = std::max(count * std::max(l.density, 0.0f) * (radius * radius * height) / area0, 1.0f);
    e.size *= std::max(l.size, 0.01f);
    // Caixa: do chao do foco ate' `height` (as folhas nascem na metade de
    // cima e caem pela de baixo).
    const bool fromTop = l.kind == AmbientLayer::Kind::Leaves;
    const float y0 = fromTop ? height * 0.5f : 0.0f;
    e.shapeSize = Vec3(radius, (height - y0) * 0.5f, radius);
    yOffset = y0 + (height - y0) * 0.5f;
    e.offset = Vec3(0.0f, yOffset, 0.0f);
    const float vida = 0.5f * (e.lifetime.x + e.lifetime.y);
    e.rate = count / vida;
    e.burstCount = static_cast<uint32_t>(count);
    e.capacity = static_cast<uint32_t>(count * 1.6f) + 16u;

    VfxEffectDesc fx;
    fx.name = "ambient";
    fx.vision = false; // cenario: a visao de equipe nao esconde o ar
    fx.emitters.push_back(e);
    return fx;
}

} // namespace

void AmbientParticles::setLayers(VfxSystem& vfx, const std::vector<AmbientLayer>& layers) {
    for (Live& l : m_live) {
        if (l.id) vfx.stop(l.id);
        if (!l.effect.empty()) vfx.forgetEffect(l.effect);
    }
    m_live.clear();
    m_layers = layers;
    m_live.resize(m_layers.size());
    ++m_generation;
}

void AmbientParticles::spawnLayer(VfxSystem& vfx, size_t i, const Vec3& focus) {
    Live& live = m_live[i];
    if (live.effect.empty()) {
        live.effect = "ambient#" + std::to_string(m_generation) + "." + std::to_string(i);
        vfx.setEffect(live.effect, buildEffect(m_layers[i], live.yOffset));
    }
    live.id = vfx.spawn(live.effect, glm::translate(Mat4(1.0f), focus));
    live.lastScale = -1.0f;
    ERUPTION_LOG_INFO("Ambiente: camada %zu (%s) id %u", i, live.effect.c_str(), live.id);
    if (!live.id) return;
    vfx.setVision(live.id, false);
    if (m_layers[i].hasColor) vfx.setColor(live.id, m_layers[i].color, true);
}

void AmbientParticles::update(VfxSystem& vfx, const Vec3& focus, float night) {
    for (size_t i = 0; i < m_layers.size(); ++i) {
        const AmbientLayer& l = m_layers[i];
        float when = 1.0f;
        if (l.when == AmbientLayer::When::Night) when = night;
        else if (l.when == AmbientLayer::When::Day) when = 1.0f - night;
        const float scale = std::clamp(m_presetScale, 0.0f, 4.0f) * when;
        Live& live = m_live[i];
        if (scale <= 0.0f && live.id == 0) continue; // nada a fazer (desligada ou fora de hora)
        if (live.id == 0 || !vfx.isPlaying(live.id)) spawnLayer(vfx, i, focus);
        if (live.id == 0) continue;
        vfx.setTransform(live.id, glm::translate(Mat4(1.0f), focus));
        if (std::abs(scale - live.lastScale) > 1e-3f) {
            // Voltou de zero (anoiteceu): reinicia para a rajada encher a caixa.
            if (live.lastScale <= 0.0f && live.lastScale > -0.5f && scale > 0.0f) vfx.restart(live.id);
            vfx.setRateScale(live.id, scale);
            live.lastScale = scale;
        }
    }
}

} // namespace eruption
