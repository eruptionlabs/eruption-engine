// Nuvens de script (tabela weather do Luau): nuvens volumetricas locais que um
// efeito de habilidade cria no ceu em cima de um alvo (raio, tempestade,
// neve forte). Reusam a camada de nuvem independente do clima (um array de
// cobertura de 128^2 com uma bolha), mas:
//  - nao dependem do clima do mapa e a troca de clima nao as apaga;
//  - nascem condensando em fade_in (dispersao 1 -> 0) e saem dispersando
//    (dispersao -> 1) em vez de so' apagar a opacidade;
//  - o spawn nao mede area (o peso so' vale para o vento, e drift=false e' o
//    padrao) e nao espera a GPU: o envio da bolha entra no comando do quadro.
#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "formats/TerrainParser.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace eruption {

namespace {

// Altura do chao em (x, z); sem mapa carregado, a do alvo da camera.
float scriptCloudGround(const LoadedMap* map, float x, float z, float fallbackY) {
    if (map) return TerrainParser::getTerrainHeightAt(map->terrain, x, z);
    return fallbackY;
}

// Faixas seguras: um valor absurdo vindo do script nao pode quebrar o shader
// nem estourar o volume de desenho.
void clampSpec(Engine::ScriptCloudSpec& s) {
    s.radiusX = glm::clamp(s.radiusX, 5.0f, 800.0f);
    s.radiusZ = glm::clamp(s.radiusZ, 5.0f, 800.0f);
    s.density = glm::clamp(s.density, 0.0f, 1.0f);
    s.coverage = glm::clamp(s.coverage, 0.0f, 1.0f);
    s.altitude = glm::clamp(s.altitude, 0.0f, 2000.0f);
    s.falloff = glm::clamp(s.falloff, 0.3f, 4.0f);
    s.darkness = glm::clamp(s.darkness, 0.0f, 1.0f);
    s.ragged = glm::clamp(s.ragged, 0.0f, 1.0f);
    s.tint = glm::clamp(s.tint, Vec3(0.0f), Vec3(4.0f));
    s.fadeIn = glm::clamp(s.fadeIn, 0.0f, 60.0f);
    s.fadeOut = glm::clamp(s.fadeOut, 0.05f, 60.0f);
    if (s.life > 3600.0f) s.life = 3600.0f;
    s.thickness = glm::clamp(s.thickness, 0.0f, 300.0f);
    s.frame = glm::clamp(s.frame, 0.0f, 1.0f);
    s.areaRadius = glm::clamp(s.areaRadius, 0.0f, 600.0f);
}

LocalCloud blobFor(const Engine::ScriptCloudSpec& s, Vec2 center, float thickness) {
    LocalCloud lc;
    (void)thickness; // mesma forma de nuvem do clima (sem domo proprio)
    lc.center = center;
    lc.radius = Vec2(s.radiusX, s.radiusZ);
    lc.density = s.density;
    lc.falloff = s.falloff;
    lc.altitude = 0.0f;
    lc.coverage = s.coverage;
    lc.rotation = 0.0f;
    lc.layerMask = 1;
    return lc;
}

// Intensidade do relampago no instante t de uma duracao dur: sobe em 12% do
// tempo, cai em curva quadratica e tremula um pouco (um relampago de verdade
// nao e' uma rampa lisa).
float flashEnvelope(float t, float dur) {
    if (dur <= 0.0f || t >= dur) return 0.0f;
    const float u = t / dur;
    const float env = u < 0.12f ? u / 0.12f : std::pow(1.0f - (u - 0.12f) / 0.88f, 2.0f);
    return env * (0.85f + 0.15f * std::cos(u * 55.0f));
}

} // namespace

float Engine::scriptCloudThickness(const ScriptCloudSpec& s) const {
    if (s.thickness > 0.0f) return s.thickness;
    return glm::clamp(0.5f * std::max(s.radiusX, s.radiusZ), 14.0f, 90.0f);
}

// Onde a massa fica no mundo. Com frame > 0 ela e' deslocada para o lado da
// camera: a visada do centro da massa (altura media = altitude + 0,4 do domo)
// cruza o chao a essa altura / tan(inclinacao) do ponto de baixo dela, entao
// recuar a massa essa distancia a faz aparecer centrada em cima do alvo.
Vec2 Engine::scriptCloudCenter(const ScriptCloudSpec& s) const {
    Vec2 c(s.x, s.z);
    if (s.frame <= 0.0f && !s.frameFit) return c;
    const Vec3 cam = m_camera.position();
    const Vec3 tgt = m_camera.target();
    const Vec2 h(cam.x - tgt.x, cam.z - tgt.z);
    const float len = glm::length(h);
    const float dy = cam.y - tgt.y;
    if (len < 1e-3f || dy < 1.0f) return c;
    if (s.frameFit) {
        // A visada do alvo cruza a altura da base a altitude * len / dy do ponto de
        // baixo dela; recuar so' essa distancia menos meio raio visivel deixa a
        // base no terco superior da tela em vez de em cima do alvo.
        const float vis = 0.7f * std::max(s.radiusX, s.radiusZ) / std::max(s.falloff, 0.01f);
        const float d = std::max(0.0f, s.altitude * len / dy - vis * 0.5f);
        return c + (h / len) * d;
    }
    const float hext = scriptCloudThickness(s) / std::max(s.falloff, 0.01f);
    const float midH = s.altitude + 0.4f * hext;
    return c + (h / len) * (midH * len / dy) * s.frame;
}

void Engine::deriveRadiiFromArea(ScriptCloudSpec& s) {
    if (s.areaRadius <= 0.0f) return;
    // A borda visivel (onde a densidade chega a zero) fica em radius / falloff.
    // Borda nominal em kScriptCloudAreaScale (2,6) x area_radius; a visivel (com o ruido) ~1,55x: com 4x a massa
    // passava de 180 m de raio e, na camera de jogo, virava tela cheia. A area
    // escurecida no chao (cloud_shadows) continua em 1,3 x area_radius, dentro da borda.
    const float r = kScriptCloudAreaScale * s.areaRadius * glm::clamp(s.falloff, 0.3f, 4.0f);
    s.radiusX = s.radiusZ = r;
}

bool Engine::scriptCloudPreset(const std::string& name, ScriptCloudSpec& s) {
    if (name == "storm") {
        // Massa preta e densa que fecha o ceu: base baixa, borda dura (falloff
        // pequeno alarga e adensa), cobertura alta.
        s.areaRadius = 45.0f; // area; a nuvem cobre bem mais (kScriptCloudAreaScale)
        s.density = 1.0f; s.coverage = 0.9f;
        s.altitude = 230.0f; s.falloff = 0.55f;
        // 0,95: medido lado a lado com 0,8 e 0,9 - com 0,8 a barriga fica cinza-claro e a massa
        // some contra o ceu; 0,95 fica escura com o topo claro e o relevo de couve-flor.
        s.darkness = 0.95f;
        s.kind = 1;
        s.fadeIn = 1.0f; s.life = 8.0f; s.fadeOut = 2.0f;
        s.frame = 0.0f;
        deriveRadiiFromArea(s);
        return true;
    }
    if (name == "snow") {
        // Nuvem clara e volumosa, de neve: sem escurecer, fria, mais gorda.
        s.areaRadius = 40.0f;
        s.density = 0.9f; s.coverage = 0.8f;
        s.altitude = 260.0f; s.falloff = 0.8f;
        s.darkness = 0.0f;
        s.kind = 2;
        s.tint = Vec3(0.94f, 0.97f, 1.0f);
        s.fadeIn = 1.0f; s.life = 8.0f; s.fadeOut = 2.0f;
        s.frame = 0.0f;
        deriveRadiiFromArea(s);
        return true;
    }
    return false;
}

// 128^2 como as outras nuvens locais; geracao no comando do proximo quadro
// (generateNow=false), sem esperar a GPU.
std::unique_ptr<CloudCoverageArray> Engine::makeScriptCloudArray(float planeY) {
    auto& global = m_cloudLayerRenderer.coverageArray();
    auto array = std::make_unique<CloudCoverageArray>();
    const uint32_t seed = 20260711u + (m_nextScriptCloudId * 7919u);
    if (!array->init(m_cloudLayerRenderer.context(), 1, 128, global.worldMin(), global.worldMax(), 0, 0.0f, seed,
                     planeY, 120.0f, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, false))
        return nullptr;
    return array;
}

void Engine::retireCloudLayer(size_t index) {
    if (index >= m_independentCloudLayers.size()) return;
    IndependentCloudLayer& l = m_independentCloudLayers[index];
    if (l.fromScript) {
        if (m_scriptCloudCount > 0) --m_scriptCloudCount;
    } else if (m_independentCloudCount > 0) {
        --m_independentCloudCount;
    }
    RetiredCloudLayer r;
    r.array = std::move(l.array);
    r.rendererId = l.rendererId;
    r.framesLeft = static_cast<int>(VulkanContext::MAX_FRAMES_IN_FLIGHT) + 1;
    m_retiredCloudLayers.push_back(std::move(r));
    m_independentCloudLayers.erase(m_independentCloudLayers.begin() + static_cast<long>(index));
}

uint32_t Engine::spawnScriptCloud(const ScriptCloudSpec& in) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!m_cloudLayersEnabled) {
        ERUPTION_LOG_WARN("weather.cloud_spawn: nuvens desligadas; nuvem recusada");
        return 0;
    }
    // Politica do teto (LocalCloud::MAX_COUNT = 100, dividido com o clima): o
    // script tem uma cota garantida de SCRIPT_QUOTA nuvens vivas. Acima dela, o
    // spawn e' recusado (id nil no Luau) com um aviso. Dentro da cota, se o teto
    // ja' esta' cheio, o clima cede: a nuvem do clima mais longe do jogador sai
    // (aposentada sem waitIdle). Sem nuvem de script nada muda no clima.
    if (m_scriptCloudCount >= LocalCloud::SCRIPT_QUOTA) {
        ERUPTION_LOG_WARN("weather.cloud_spawn: cota de nuvens de script cheia (%u); nuvem recusada",
                          LocalCloud::SCRIPT_QUOTA);
        return 0;
    }
    if (m_independentCloudCount + m_scriptCloudCount >= LocalCloud::MAX_COUNT) {
        const Vec2 ref(m_camera.target().x, m_camera.target().z);
        size_t far = m_independentCloudLayers.size();
        float farDist = -1.0f;
        for (size_t i = 0; i < m_independentCloudLayers.size(); ++i) {
            const IndependentCloudLayer& l = m_independentCloudLayers[i];
            if (l.fromScript) continue;
            const float d = glm::distance(l.centerXZ, ref);
            if (d > farDist) { farDist = d; far = i; }
        }
        if (far == m_independentCloudLayers.size()) {
            ERUPTION_LOG_WARN("weather.cloud_spawn: teto de %u nuvens cheio e nenhuma do clima para ceder; nuvem recusada",
                              LocalCloud::MAX_COUNT);
            return 0;
        }
        retireCloudLayer(far);
    }

    ScriptCloudSpec spec = in;
    clampSpec(spec);
    const Vec2 center = scriptCloudCenter(spec);
    const float planeY = scriptCloudGround(m_currentMap.get(), center.x, center.y, m_camera.target().y) + spec.altitude;

    std::unique_ptr<CloudCoverageArray> array;
#if ERUPTION_DEV
    // ERUPTION_TEST_CLOUD_NO_SPARE=1: ignora o estoque de arrays (cria um novo a cada
    // spawn), para isolar defeito do array reaproveitado.
    static const bool kNoSpare = std::getenv("ERUPTION_TEST_CLOUD_NO_SPARE") != nullptr;
#else
    constexpr bool kNoSpare = false;
#endif
    if (!kNoSpare && !m_spareCloudArrays.empty()) {
        array = std::move(m_spareCloudArrays.back());
        m_spareCloudArrays.pop_back();
        array->setSeed(20260711u + (m_nextScriptCloudId * 7919u));
    } else {
        array = makeScriptCloudArray(planeY);
        if (!array) {
            ERUPTION_LOG_ERROR("weather.cloud_spawn: falha ao criar a camada da nuvem");
            return 0;
        }
    }
    m_scriptCloudsUsed = true;
    const auto tInit = std::chrono::steady_clock::now();
    array->addLocalCloud(blobFor(spec, center, scriptCloudThickness(spec)));
    CloudCoverageArray* raw = array.get();
    const uint32_t rid = m_cloudLayerRenderer.registerCloudLayer(raw);
    if (rid == UINT32_MAX) {
        ERUPTION_LOG_ERROR("weather.cloud_spawn: sem camada de nuvem livre no renderizador");
        return 0;
    }
    // Sem chuva/tempestade do clima: o escurecimento e' o darkness do script.
    m_cloudLayerRenderer.setCloudLayerWeather(rid, 0.0f, 0.0f);

    IndependentCloudLayer layer;
    layer.array = std::move(array);
    layer.altitude = 0.5f;
    layer.planeY = planeY;
    layer.centerXZ = center;
    layer.density = spec.density;
    // Peso so' para o vento (drift=true): sem a medicao de area, que custava
    // caro e nao tem o que medir numa bolha so'.
    layer.weight = 1.0f + 2.5f * spec.density;
    layer.rendererId = rid;
    layer.fromScript = true;
    layer.scriptId = m_nextScriptCloudId++;
    layer.spec = spec;
    layer.dispersal = (spec.fadeIn > 1e-3f) ? 1.0f : 0.0f;
    applyScriptCloudLook(layer);
    const uint32_t id = layer.scriptId;
#if ERUPTION_DEV
    const float msInit = std::chrono::duration<float, std::milli>(tInit - t0).count();
#endif
    m_independentCloudLayers.push_back(std::move(layer));
    ++m_scriptCloudCount;
#if ERUPTION_DEV
    // ERUPTION_TEST_CLOUD_SCRIPT_LOG=1: custo do spawn em ms (bancada).
    static const bool kLog = std::getenv("ERUPTION_TEST_CLOUD_SCRIPT_LOG") != nullptr;
    if (kLog)
        ERUPTION_LOG_WARN("[NUVEM-SCRIPT] spawn id=%u em %.3f ms (init do array %.3f; vivas=%u, clima=%u)", id,
                          std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                          msInit, m_scriptCloudCount, m_independentCloudCount);
#else
    (void)t0;
#endif
    return id;
}

void Engine::applyScriptCloudLook(IndependentCloudLayer& layer) {
    const float flash = layer.flashPeak * flashEnvelope(layer.flashT, layer.flashDur);
    m_cloudLayerRenderer.setCloudLayerLook(layer.rendererId, layer.spec.tint, layer.spec.darkness, layer.dispersal,
                                           flash, layer.spec.ragged);
}

bool Engine::setScriptCloud(uint32_t id, const ScriptCloudSpec& in, uint32_t fields) {
    for (IndependentCloudLayer& l : m_independentCloudLayers) {
        if (!l.fromScript || l.scriptId != id) continue;
        ScriptCloudSpec s = l.spec;
        if (fields & ScriptCloudSpec::FX) s.x = in.x;
        if (fields & ScriptCloudSpec::FZ) s.z = in.z;
        if (fields & ScriptCloudSpec::FRadiusX) s.radiusX = in.radiusX;
        if (fields & ScriptCloudSpec::FRadiusZ) s.radiusZ = in.radiusZ;
        if (fields & ScriptCloudSpec::FDensity) s.density = in.density;
        if (fields & ScriptCloudSpec::FCoverage) s.coverage = in.coverage;
        if (fields & ScriptCloudSpec::FAltitude) s.altitude = in.altitude;
        if (fields & ScriptCloudSpec::FFalloff) s.falloff = in.falloff;
        if (fields & ScriptCloudSpec::FDarkness) s.darkness = in.darkness;
        if (fields & ScriptCloudSpec::FRagged) s.ragged = in.ragged;
        if (fields & ScriptCloudSpec::FTint) s.tint = in.tint;
        if (fields & ScriptCloudSpec::FFadeIn) s.fadeIn = in.fadeIn;
        if (fields & ScriptCloudSpec::FLife) s.life = in.life;
        if (fields & ScriptCloudSpec::FFadeOut) s.fadeOut = in.fadeOut;
        if (fields & ScriptCloudSpec::FDrift) s.drift = in.drift;
        if (fields & ScriptCloudSpec::FPrecip) s.precip = in.precip;
        if (fields & ScriptCloudSpec::FClimate) s.climate = in.climate;
        if (fields & ScriptCloudSpec::FThickness) s.thickness = in.thickness;
        if (fields & ScriptCloudSpec::FFrame) s.frame = in.frame;
        if (fields & ScriptCloudSpec::FFrameFit) s.frameFit = in.frameFit;
        if (fields & ScriptCloudSpec::FArea) {
            s.areaRadius = in.areaRadius;
            if (!(fields & (ScriptCloudSpec::FRadiusX | ScriptCloudSpec::FRadiusZ))) deriveRadiiFromArea(s);
        }
        clampSpec(s);

        const ScriptCloudSpec& o = l.spec;
        const Vec2 center = scriptCloudCenter(s);
        const float thick = scriptCloudThickness(s);
        // So' o que muda a bolha (e a cobertura/sombra) refaz o envio; escurecer
        // e tingir e' parametro de shader e nao custa nada.
        const bool shapeChanged = s.x != o.x || s.z != o.z || s.radiusX != o.radiusX || s.radiusZ != o.radiusZ ||
                                  s.density != o.density || s.coverage != o.coverage || s.falloff != o.falloff ||
                                  s.thickness != o.thickness || s.frame != o.frame || s.frameFit != o.frameFit;
        if (shapeChanged) {
            // Com drift ligado o vento desloca a nuvem pelo offset de UV: a bolha
            // guarda a posicao menos esse deslocamento para (x, z) valer como dito.
            const Vec3 wsz = l.array->worldMax() - l.array->worldMin();
            const Vec2 off = l.array->windOffset() * Vec2(wsz.x, wsz.z);
            l.array->setLocalCloud(0, blobFor(s, center - off, thick));
            l.centerXZ = center - off;
            l.density = s.density;
            l.weight = 1.0f + 2.5f * s.density;
        }
        if (shapeChanged || s.altitude != o.altitude)
            l.planeY = scriptCloudGround(m_currentMap.get(), center.x, center.y, m_camera.target().y) + s.altitude;
        l.spec = s;
        applyScriptCloudLook(l);
        return true;
    }
    return false;
}

bool Engine::moveScriptCloud(uint32_t id, float x, float z) {
    ScriptCloudSpec s;
    s.x = x;
    s.z = z;
    return setScriptCloud(id, s, ScriptCloudSpec::FX | ScriptCloudSpec::FZ);
}

bool Engine::removeScriptCloud(uint32_t id, float fadeOut) {
    for (IndependentCloudLayer& l : m_independentCloudLayers) {
        if (!l.fromScript || l.scriptId != id) continue;
        if (l.removing) return true; // ja' dispersando: a primeira chamada manda
        l.removing = true;
        l.removeT = 0.0f;
        l.removeDur = std::max(fadeOut >= 0.0f ? fadeOut : l.spec.fadeOut, 0.05f);
        // Parte do estado em que a nuvem esta': no meio do fade_in ela dispersa
        // dali, sem pular e sem voltar a crescer.
        l.removeFrom = l.dispersal;
        return true;
    }
    return false;
}

void Engine::updateScriptClouds(float dt) {
    for (size_t i = 0; i < m_retiredCloudLayers.size();) {
        if (--m_retiredCloudLayers[i].framesLeft <= 0) {
            m_cloudLayerRenderer.unregisterCloudLayer(m_retiredCloudLayers[i].rendererId);
            m_retiredCloudLayers.erase(m_retiredCloudLayers.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
    // Repoe o estoque de arrays (ver m_spareCloudArrays): so' depois de o script ter
    // usado nuvens, e um por 0,1 s, para o custo de criar nao cair num quadro so'.
    m_spareRefillCooldown -= dt;
    if (m_scriptCloudsUsed && m_spareCloudArrays.size() < 4 && m_spareRefillCooldown <= 0.0f) {
        m_spareRefillCooldown = 0.1f;
        if (auto a = makeScriptCloudArray(0.0f)) m_spareCloudArrays.push_back(std::move(a));
    }
    if (m_scriptCloudCount == 0) return;
    for (size_t i = 0; i < m_independentCloudLayers.size();) {
        IndependentCloudLayer& l = m_independentCloudLayers[i];
        if (!l.fromScript) { ++i; continue; }
        l.age += dt;
        // Chuva/neve automatica do preset: a tempestade usa a chuva do clima (seguidor de
        // chuva, as mesmas gotas e fitas), a neve os flocos da nuvem. cloud_precip, quando
        // chamado, manda no lugar.
        if (!l.precipExplicit) {
            const bool on = l.spec.precip && l.spec.kind > 0;
            l.hasRain = on && l.spec.kind == 1;
            l.precipOn = on && l.spec.kind == 2;
            l.precipSnow = true;
        }
        if (l.flashDur > 0.0f) {
            l.flashT += dt;
            if (l.flashT >= l.flashDur) { l.flashDur = 0.0f; l.flashPeak = 0.0f; l.flashT = 0.0f; }
        }
        if (!l.removing) {
            const float k = (l.spec.fadeIn > 1e-3f) ? glm::clamp(l.age / l.spec.fadeIn, 0.0f, 1.0f) : 1.0f;
            l.dispersal = 1.0f - k * k * (3.0f - 2.0f * k); // condensa com saida suave
            if (l.spec.life > 0.0f && l.age >= l.spec.fadeIn + l.spec.life) {
                l.removing = true;
                l.removeT = 0.0f;
                l.removeDur = std::max(l.spec.fadeOut, 0.05f);
                l.removeFrom = l.dispersal;
            }
        }
        if (l.removing) {
            l.removeT += dt;
            const float k = glm::clamp(l.removeT / l.removeDur, 0.0f, 1.0f);
            l.dispersal = l.removeFrom + (1.0f - l.removeFrom) * k;
            if (k >= 1.0f) {
                retireCloudLayer(i);
                continue;
            }
        }
        applyScriptCloudLook(l);
        ++i;
    }
}

bool Engine::flashScriptCloud(uint32_t id, float intensity, float duration) {
    for (IndependentCloudLayer& l : m_independentCloudLayers) {
        if (!l.fromScript || l.scriptId != id) continue;
        l.flashPeak = glm::clamp(intensity, 0.0f, 8.0f);
        l.flashDur = std::max(duration, 0.02f);
        l.flashT = 0.0f;
        return true;
    }
    return false;
}

// Ponto no miolo do volume (onde a densidade e' alta): perto do centro em x e z
// e no meio da altura do domo; e' dali que o raio nasce.
bool Engine::scriptCloudPoint(uint32_t id, Vec3& out) {
    for (const IndependentCloudLayer& l : m_independentCloudLayers) {
        if (!l.fromScript || l.scriptId != id) continue;
        const Vec3 wsz = l.array->worldMax() - l.array->worldMin();
        const Vec2 wc = l.centerXZ + l.array->windOffset() * Vec2(wsz.x, wsz.z);
        auto rnd = [this]() {
            m_scriptCloudRng ^= m_scriptCloudRng << 13;
            m_scriptCloudRng ^= m_scriptCloudRng >> 17;
            m_scriptCloudRng ^= m_scriptCloudRng << 5;
            return float(m_scriptCloudRng & 0xFFFFFFu) / float(0x1000000u);
        };
        const float fall = std::max(l.spec.falloff, 0.01f);
        const float hext = scriptCloudThickness(l.spec) / fall;
        const float a = rnd() * 6.2831853f;
        const float rr = std::sqrt(rnd()) * 0.35f;
        const float ry = glm::clamp(std::max(l.spec.radiusX, l.spec.radiusZ) / fall * 0.6f, 6.0f, 100.0f);
        (void)hext;
        out = Vec3(wc.x + std::cos(a) * rr * l.spec.radiusX / fall, l.planeY + ry * (0.05f + 0.2f * rnd()),
                   wc.y + std::sin(a) * rr * l.spec.radiusZ / fall);
        return true;
    }
    return false;
}

bool Engine::setScriptCloudPrecip(uint32_t id, bool on, bool snow, float rate, float radius) {
    for (IndependentCloudLayer& l : m_independentCloudLayers) {
        if (!l.fromScript || l.scriptId != id) continue;
        l.precipExplicit = true;
        l.precipOn = on;
        l.precipSnow = snow;
        l.precipRate = glm::clamp(rate, 0.0f, 3.0f);
        l.precipRadius = glm::clamp(radius, 0.0f, 600.0f);
        return true;
    }
    return false;
}

uint32_t Engine::startBolt(BoltSpec spec) {
    if (spec.cloudId) {
        bool found = false;
        for (const IndependentCloudLayer& l : m_independentCloudLayers) {
            if (l.fromScript && l.scriptId == spec.cloudId) {
                spec.cloudBaseY = l.planeY;
                const Vec3 wsz = l.array->worldMax() - l.array->worldMin();
                const Vec2 wc = l.centerXZ + l.array->windOffset() * Vec2(wsz.x, wsz.z);
                const float fall = std::max(l.spec.falloff, 0.01f);
                // Miolo solido: ~0,6 do semi-eixo nominal na horizontal; a altura e' o
                // domo (espessura / falloff).
                spec.cloudCenter = Vec3(wc.x, l.planeY, wc.y);
                spec.cloudRadii = Vec3(0.6f * l.spec.radiusX / fall, scriptCloudThickness(l.spec) / fall,
                                       0.6f * l.spec.radiusZ / fall);
                if (spec.fromInside) {
                    auto rnd = [this]() {
                        m_scriptCloudRng ^= m_scriptCloudRng << 13;
                        m_scriptCloudRng ^= m_scriptCloudRng >> 17;
                        m_scriptCloudRng ^= m_scriptCloudRng << 5;
                        return float(m_scriptCloudRng & 0xFFFFFFu) / float(0x1000000u);
                    };
                    // Ponto sorteado dentro da base da massa (disco uniforme, logo acima do plano).
                    const float a = rnd() * 6.2831853f;
                    const float rr = std::sqrt(rnd()) * 0.55f;
                    spec.from = Vec3(spec.cloudCenter.x + std::cos(a) * rr * spec.cloudRadii.x,
                                     l.planeY + spec.cloudRadii.y * (0.06f + 0.14f * rnd()),
                                     spec.cloudCenter.z + std::sin(a) * rr * spec.cloudRadii.z);
                }
                found = true;
                break;
            }
        }
        if (!found) spec.cloudId = 0;
        if (!found) spec.fromInside = false;
    }
    return m_bolts.start(spec);
}

void Engine::updateScriptFx(float dt) {
    m_bolts.update(dt);
    updateTornados(dt);
    static std::vector<CloudPrecip::Params> emitters;
    emitters.clear();
    for (const IndependentCloudLayer& l : m_independentCloudLayers) {
        if (!l.fromScript || !l.precipOn) continue;
        CloudPrecip::Params p;
        p.id = l.scriptId;
        const Vec3 wsz = l.array->worldMax() - l.array->worldMin();
        p.center = l.centerXZ + l.array->windOffset() * Vec2(wsz.x, wsz.z);
        p.baseY = l.planeY;
        const float fall = std::max(l.spec.falloff, 0.01f);
        p.radius = l.precipRadius > 0.0f ? l.precipRadius
                                         : (l.spec.areaRadius > 0.0f ? l.spec.areaRadius
                                                                     : 0.7f * std::max(l.spec.radiusX, l.spec.radiusZ) / fall);
        p.snow = l.precipSnow;
        p.rate = l.precipRate;
        p.alpha = 1.0f - glm::smoothstep(0.1f, 0.7f, l.dispersal);
        emitters.push_back(p);
    }
    if (emitters.empty() && m_precip.particleCount() == 0) return;
    const LoadedMap* map = m_currentMap.get();
    const float fallback = m_camera.target().y;
    m_precip.update(dt, emitters, [map, fallback](float x, float z) {
        return map ? TerrainParser::getTerrainHeightAt(map->terrain, x, z) : fallback;
    });
}

// CLIMA LOCAL. Um clima de chuva ou neve muda o mundo todo porque a luz direta do sol
// some e sobra a do ceu coberto. Aqui o mesmo, mas so' em volta da nuvem. Nada e'
// inventado: os parametros saem do template do proprio clima (Stormy para a nuvem de
// tempestade, Snowy para a de neve) e das mesmas contas do motor:
//  - oclusao do sol: updateCoherence (cobertura^1,2 x espessura x curva, e a tempestade
//    esconde o resto);
//  - exposicao: a mesma razao luz-do-sol/ambiente que a exposicao automatica usa
//    (sol x 0,7 + ambiente x 0,9), com e sem o sol ocluido;
//  - a dessaturacao e' a parte da luz que era direta e sumiu; o que sobra tem a cor do
//    ceu coberto (o humor da tempestade do weather_overlay; a neblina do ciclo na neve);
//  - neblina: fog_density, inicio e fim do clima.
// O peso por nuvem sobe/desce com a formacao e a dispersao dela (a mesma curva da chuva).
void Engine::fillLocalClimate(PostProcessor::PostSettings& out) const {
    PostProcessor::PostSettings::LocalClimate& lc = out.localClimate;
    lc = PostProcessor::PostSettings::LocalClimate{};
    if (m_scriptCloudCount == 0) return;
    const float sun = m_dayNightCycle.getSunIntensity();
    const float direct = std::max(sun, 0.0f) * 0.7f;
    const float ambient = m_ambientIntensity * 0.9f;
    const Vec3 fog = m_dayNightCycle.getFogColor();
    const float kLuma[3] = {0.2126f, 0.7152f, 0.0722f};
    const WeatherType kinds[2] = {WeatherType::Stormy, WeatherType::Snowy};
    for (int k = 0; k < 2; ++k) {
        const WeatherParams p = m_weatherSystem.templateFor(kinds[k]);
        const float coverage = std::pow(p.cloudCoverage, 1.2f) * p.cloudThickness * m_weatherSystem.cloudOcclusionCurve();
        float sunOcc = 1.0f - glm::clamp(coverage, 0.0f, 1.0f);
        sunOcc *= 1.0f - glm::clamp(p.stormTint * 1.2f, 0.0f, 0.95f);
        const float lit = direct + ambient;
        const float exposure = lit > 1e-4f ? (direct * sunOcc + ambient) / lit : 1.0f;
        const float lostShare = lit > 1e-4f ? direct * (1.0f - sunOcc) / lit : 0.0f;
        lc.grade[k] = Vec4(exposure, lostShare, k == 0 ? p.stormTint * 0.10f : 0.0f, p.fogDensity);
        lc.fog[k] = Vec4(p.fogStart, p.fogEnd, 0.0f, 0.0f);
        // Cor do ceu coberto, normalizada pela luminancia (so' a matiz).
        Vec3 sky = k == 0 ? Vec3(0.18f, 0.18f, 0.22f) : fog;
        const float l = std::max(sky.x * kLuma[0] + sky.y * kLuma[1] + sky.z * kLuma[2], 1e-4f);
        lc.chroma[k] = Vec4(sky / l, 0.0f);
    }
    for (const IndependentCloudLayer& l : m_independentCloudLayers) {
        if (!l.fromScript || l.spec.kind <= 0 || !l.spec.climate) continue;
        if (lc.count >= PostProcessor::PostSettings::kMaxLocalClimate) break;
        const float strength = 1.0f - glm::smoothstep(0.1f, 0.7f, l.dispersal);
        if (strength <= 0.0f) continue;
        const Vec3 wsz = l.array->worldMax() - l.array->worldMin();
        const Vec2 c = l.centerXZ + l.array->windOffset() * Vec2(wsz.x, wsz.z);
        const float fall = std::max(l.spec.falloff, 0.01f);
        const float area = l.spec.areaRadius > 0.0f ? l.spec.areaRadius
                                                    : 0.7f * std::max(l.spec.radiusX, l.spec.radiusZ) / fall;
        const float radius = 2.0f * area;
        lc.cloud[lc.count] = Vec4(c.x, c.y, l.spec.kind == 2 ? -radius : radius, strength);
        lc.height[lc.count] = l.planeY;
        ++lc.count;
    }
}

void Engine::submitScriptFx() {
    if (!m_immediateDraw.ready()) return;
    m_bolts.submit(m_immediateDraw, m_camera.position());
    if (m_tornados.active() && m_tornadoDebris > 0) {
        const float level = m_dayNightCycle.getSunIntensity() * 0.7f + m_ambientIntensity * 0.9f;
        m_tornados.submitDebris(m_immediateDraw, uint32_t(m_tornadoDebris), glm::clamp(level / 1.55f, 0.1f, 1.0f));
    }
    // Floco iluminado: albedo de neve x o nivel de luz da cena (a mesma conta da exposicao
    // automatica), reduzido pela mesma queda de luz do clima local da neve.
    const float level = m_dayNightCycle.getSunIntensity() * 0.7f + m_ambientIntensity * 0.9f;
    const float lit = 0.92f * glm::clamp(level / 1.55f, 0.1f, 1.0f);
    m_precip.submit(m_immediateDraw, m_camera.position(), m_camera.right(), m_camera.up(),
                    Vec3(0.95f, 0.97f, 1.0f) * lit);
}

} // namespace eruption
