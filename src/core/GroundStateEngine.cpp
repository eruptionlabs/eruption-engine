// Cola do estado do chao por celula (ver core/GroundState.hpp): pede o chao ao
// motor, mantem a janela em volta da camera, refaz a imagem em baixa resolucao
// ~5 vezes por segundo (so' enquanto ha' estado vivo) e a entrega ao passe de
// iluminacao (binding 20) e a lista de gelo ao shader da agua.
//
// Custo com nada vivo: zero (nem a textura e' lida: groundStateWin.w = 0).
// Com marcas vivas: a CPU refaz so' as caixas delas e sobe o retangulo sujo.
#include "core/Engine.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace eruption {

namespace {

// Padroes por tipo, em segundos: quanto dura e quanto desse tempo gasta sumindo.
struct KindDefaults { float duration, fade; };
KindDefaults defaultsFor(ground::Kind k) {
    switch (k) {
    case ground::Kind::Scorch: return {60.0f, 45.0f};
    case ground::Kind::Frost: return {30.0f, 18.0f};
    case ground::Kind::Wet: return {30.0f, 24.0f};
    case ground::Kind::Snow: return {90.0f, 45.0f};
    case ground::Kind::IceWater: return {45.0f, 20.0f};
    }
    return {30.0f, 20.0f};
}

} // namespace

void Engine::groundStateApplyPreset(const json& p) {
    m_groundTexels = uint32_t(std::clamp(p.value("ground_state_texels", 512), 64, 1024));
    m_groundTexelM = std::clamp(p.value("ground_state_texel_m", 0.5f), 0.1f, 4.0f);
    m_groundCap = uint32_t(std::clamp(p.value("ground_state_max", 400), 1, int(ground::kMaxStates)));
}

void Engine::groundStateOnMapLoaded() {
    m_groundField.clear();
    m_groundDirty = true;
}

uint32_t Engine::groundState(const GroundStateRequest& r) {
    ground::Kind kind;
    if (!ground::kindFromName(r.kind, kind)) {
        ERUPTION_LOG_WARN("world.ground_state: tipo desconhecido '%s' (scorch, frost, wet, snow, ice_water)", r.kind.c_str());
        return 0;
    }
    const float upm = std::max(m_vfx.unitsPerMeter(), 1e-4f);
    ground::State s;
    s.kind = kind;
    s.x = r.x;
    s.z = r.z;
    s.radius = std::clamp(r.radius > 0.0f ? r.radius : 3.0f * upm, 0.05f * upm, 2000.0f);
    s.square = r.square;
    s.strength = std::clamp(r.strength, 0.0f, 1.0f);
    const KindDefaults d = defaultsFor(kind);
    s.duration = std::clamp(r.duration > 0.0f ? r.duration : d.duration, 0.1f, 36000.0f);
    // fade < 0: o padrao do tipo, no mesmo teto da duracao; fade = 0 some de repente.
    s.fade = r.fade >= 0.0f ? r.fade : std::min(d.fade * s.duration / d.duration, s.duration);
    s.born = m_affectClock;
    if (++m_affectNextId == 0) ++m_affectNextId;
    const uint32_t id = m_affectNextId;
    const float y0 = floorHeight(r.x, r.z, FLT_MAX);
    const float from = y0 + 2.5f * upm;
    // So' o chao rente (terreno e modelos de chao): o que fica a mais de 2,5 m
    // acima do centro (telhado, ponte) nao entra na altura de referencia.
    auto groundY = [this, from](float x, float z) { return floorHeight(x, z, from); };
    m_groundField.add(std::move(s), id, groundY, m_groundCap);
    m_groundDirty = true;
    m_groundNextRaster = 0.0; // sobe ja' no proximo quadro
    return id;
}

bool Engine::groundStateStop(uint32_t id) {
    if (!m_groundField.stop(id)) return false;
    m_groundDirty = true;
    return true;
}

void Engine::updateGroundState(float dt) {
    (void)dt;
    ground::Field& f = m_groundField;
    const float upm = std::max(m_vfx.unitsPerMeter(), 1e-4f);
    const ground::Params want{m_groundTexels, m_groundTexelM * upm};
    if (f.params().texels != want.texels || std::fabs(f.params().texelWorld - want.texelWorld) > 1e-4f) {
        f.configure(want);
        m_groundDirty = true;
    }
    if (m_deferredLighting.groundStateTexels() != want.texels) {
        m_deferredLighting.configureGroundState(want.texels);
        m_groundDirty = true;
    }
    bool lost = m_deferredLighting.takeGroundStateLost();
    if (f.tick(m_affectClock)) m_groundDirty = true;
    const Vec3 focus = m_camera.target();
    if (f.follow(focus.x, focus.z)) m_groundDirty = true;
    const bool idle = f.size() == 0 && !f.lit();
    if (idle) {
        m_deferredLighting.setGroundStateParams(Vec4(0.0f), Vec4(0.0f));
        m_water.setIceAreas(nullptr, 0);
        return;
    }
    // Marcas novas sobem depressa (a rampa de entrada dura 0,5 s); o resto, so'
    // a cada 0,2 s: o sumir leva dezenas de segundos.
    bool young = false;
    for (const ground::State& s : f.states())
        if (m_affectClock - s.born < 0.7) young = true;
    // Em regime (so' segurando) nada muda: nao refaz nem sobe (custo ~0 com 400 celulas paradas).
    if (lost || m_groundDirty || (m_affectClock >= m_groundNextRaster && f.animating(m_affectClock))) {
        ground::Rect rect = f.rasterize(m_affectClock);
        if (lost) rect = ground::Rect{0, 0, int(want.texels) - 1, int(want.texels) - 1};
        if (rect.valid()) m_deferredLighting.queueGroundStateUpload(f.pixels().data(), rect.x0, rect.y0, rect.x1, rect.y1);
        m_groundDirty = false;
        m_groundNextRaster = m_affectClock + (young ? 0.05 : 0.2);
    }
    const float size = f.windowSize();
    m_deferredLighting.setGroundStateParams(
        Vec4(f.originX(), f.originZ(), 1.0f / size, f.lit() ? 1.0f : 0.0f),
        Vec4(f.heightBase(), f.heightRange(), 2.0f * upm, 1.0f * upm));
    float areas[ground::kMaxIceAreas][4];
    const uint32_t n = f.iceAreas(m_affectClock, areas, ground::kMaxIceAreas);
    m_water.setIceAreas(&areas[0][0], n);
}

} // namespace eruption
