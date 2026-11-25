#include "core/Engine.hpp"
#include "core/EngineInternal.hpp"
#include "core/Logger.hpp"
#include "utils/Profiler.hpp"
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace eruption {

namespace {

// Plano de recorte proximo obliquo (Lengyel, "Oblique View Frustum Depth
// Projection and Clipping", JGT 2005), na convencao do motor: z/w em [0, 1]
// (GLM_FORCE_DEPTH_ZERO_TO_ONE). `clip` leva o ponto de MUNDO ao recorte; a
// linha z vira a*C, com C o plano em mundo (C.p >= 0 fica) e a escolhido para
// o canto distante do frustum Q (sinal de C no recorte, z = w = 1) continuar
// em z = w. A geometria do lado negativo do plano (embaixo d'agua, que o
// espelho levaria para a frente do reflexo) sai pelo recorte de hardware,
// sem mudar nenhum shader do G-buffer. Precisao dupla: o plano quase paralelo
// ao raio de visao deixa C.Q pequeno. false = matriz degenerada (fica a de
// antes).
bool obliqueNearPlane(Mat4& clip, const Vec4& planeWorld) {
    const glm::dmat4 A(clip);
    const glm::dvec4 C(planeWorld);
    const glm::dvec4 Cc = glm::transpose(glm::inverse(A)) * C;
    const glm::dvec4 Qc((Cc.x > 0.0) - (Cc.x < 0.0), (Cc.y > 0.0) - (Cc.y < 0.0), 1.0, 1.0);
    const glm::dvec4 Q = glm::inverse(A) * Qc;
    const glm::dvec4 rowW(A[0][3], A[1][3], A[2][3], A[3][3]);
    const double cq = glm::dot(C, Q);
    if (!std::isfinite(cq) || std::fabs(cq) < 1e-12) return false;
    const double a = glm::dot(rowW, Q) / cq;
    if (!std::isfinite(a) || a <= 0.0) return false;
    for (int c = 0; c < 4; ++c) clip[c][2] = static_cast<float>(a * C[c]);
    return true;
}

} // namespace

bool Engine::waterInViewThisFrame() const {
    return m_renderEffects[7]->isEnabled() && (waterPlaneInView() || contourWaterInView());
}

// NUVENS NO REFLEXO. A agua roda antes do passe das nuvens: o SSPR copia a
// imagem acesa sem nuvem e o G-buffer espelhado so' tem geometria, entao o
// lago refletia o ceu limpo com a nuvem bem em cima dele. Aqui as mesmas
// nuvens volumetricas (as bolhas de cada camada, o mesmo cloud_billboard)
// sao desenhadas pela camera espelhada no acumulo de 1/6 da cena - o mesmo
// alvo e o mesmo tamanho do passe principal, que o limpa de novo mais tarde
// no quadro. O raio sai da camera espelhada e atravessa a caixa REAL da
// nuvem: e' o raio refletido, entao posicao, forma e cor sao as da nuvem do
// ceu. A profundidade do G-buffer espelhado corta a nuvem atras do que o
// espelho desenhou. O composite poe o resultado por cima da cena espelhada.
// rasterVp: espelho sem o plano obliquo (as nuvens estao todas acima d'agua;
// so' a linha z muda, o pixel e' o mesmo); depthVp: a matriz que gravou a
// profundidade. Devolve a view do acumulo, ou nula sem nuvem no reflexo.
VkImageView Engine::renderWaterReflectionClouds(VkCommandBuffer cmd, const Mat4& depthVp, const Mat4& rasterVp,
                                                const Mat4& cullVp, const Vec3& mirroredCam) {
    // ERUPTION_TEST_NO_REFLECTION_CLOUDS=1 (teste): reflexo sem as nuvens,
    // para o A/B de imagem e de custo com o mesmo binario.
    static const bool kOff = std::getenv("ERUPTION_TEST_NO_REFLECTION_CLOUDS") != nullptr;
    if (kOff || !m_cloudLayersEnabled || m_independentCloudLayers.empty()) return VK_NULL_HANDLE;
    GBuffer& gb = m_waterReflection.gbuffer();
    gb.transitionToRead(cmd); // a profundidade espelhada vira textura (o composite repete: no-op)
    const LightingEnvironment& env = m_deferredLighting.environment();
    CloudLayerRenderer::MirrorView view;
    view.viewProj = rasterVp;
    view.depthViewProj = depthVp;
    view.cullViewProj = cullVp;
    view.cameraPos = mirroredCam;
    view.sunDir = env.sun.direction;
    view.sunIntensity = env.sun.intensity;
    view.time = m_timer.elapsed();
    view.frameIndex = static_cast<uint32_t>(m_framesCount);
    view.depthView = gb.depthView();
    // Mesmo tamanho do acumulo do passe principal (Render.cpp): tamanho
    // diferente recriaria a imagem duas vezes por quadro.
    const uint32_t accW = (renderExtent().width + 5) / 6;
    const uint32_t accH = (renderExtent().height + 5) / 6;
    if (!m_cloudLayerRenderer.beginMirrorView(cmd, view, accW, accH)) return VK_NULL_HANDLE;
    // Ordem de pintura do passe principal (a do quadro anterior, de longe
    // para perto): a camera espelhada tem o mesmo x/z, a ordem quase nao muda.
    std::array<const IndependentCloudLayer*, CloudLayerRenderer::MAX_CLOUD_LAYERS> byId{};
    for (const IndependentCloudLayer& icl : m_independentCloudLayers)
        if (icl.rendererId < byId.size()) byId[icl.rendererId] = &icl;
    auto draw = [&](const IndependentCloudLayer* icl) {
        if (m_postSettings.debugRainRegions && icl->hasRain) return; // o passe principal tambem pula
        m_cloudLayerRenderer.renderMirrorBillboard(cmd, icl->rendererId, icl->planeY);
    };
    for (uint32_t id : m_cloudLayerDrawOrder) {
        if (id >= byId.size() || !byId[id]) continue;
        draw(byId[id]);
        byId[id] = nullptr;
    }
    for (const IndependentCloudLayer* icl : byId)
        if (icl) draw(icl);
    return m_cloudLayerRenderer.endMirrorView(cmd);
}

// PERSONAGENS NO REFLEXO. O sprite e' um quadro virado para a camera (e
// inclinado com ela); o SSPR nao o reflete (sem oclusao: o personagem numa
// pedra da margem refletia na agua la' embaixo, flutuando). Aqui cada billboard
// visivel vira um cartao em pe' na ancora, de frente para a camera na
// horizontal, desenhado em mundo pela MESMA matriz espelhada, no mesmo
// G-buffer e com a mesma profundidade e plano de recorte dos modelos: a pedra
// o esconde como esconde uma arvore, e quem esta' na agua ou na areia reflete
// colado ao pe', sem desvanecer. Limite: so' entram os sprites visiveis na
// camera principal (o culling de sprite e' o dela).
void Engine::drawWaterReflectionSprites(VkCommandBuffer cmd, const Mat4& drawVp) {
    const uint32_t count = m_spriteSystem.visibleCount();
    if (count == 0) return;
    // viewRight de sprite_vert.glsl = -X da camera; na horizontal. O sentido ate'
    // a camera sai do produto vetorial (vale mesmo olhando reto para baixo).
    const Mat4 invView = glm::inverse(m_camera.viewMatrix());
    Vec3 right = -Vec3(invView[0]);
    right.y = 0.0f;
    if (glm::dot(right, right) < 1e-8f) return;
    right = glm::normalize(right);
    const Vec3 facing = -glm::normalize(glm::cross(right, Vec3(0.0f, 1.0f, 0.0f)));
    // Altura inteira: a ancora abaixo do nivel sobe ate' ele (sprite_mirror.vert).
    float level = 0.0f;
    const bool full = m_water.style().spriteReflectFull && m_water.planarReflectionPlane(m_waterMenu.config, level);
    m_spriteRenderer.renderMirror(cmd, drawVp, right, facing, kSpriteNormalYMix, m_spriteSystem.instanceBuffer(), count,
                                  full ? level : -1.0e30f);
}

// REFLEXO ESPELHADO (WaterReflectionPass). O SSPR so' espelha pixels da tela:
// a barraca acima da borda de cima nao tem pixel, e o reflexo dela sumia da
// agua no meio da tela. Aqui a cena e' desenhada de novo pela camera
// espelhada no plano d'agua - matriz viewProj * espelho, culling pelo frustum
// dessa matriz (o que tem o REFLEXO na tela entra, esteja o objeto onde
// estiver) - num G-buffer proprio de resolucao reduzida, e o composite acende
// com sol e ambiente. A agua usa isto so' onde o SSPR nao achou nada.
//  - o espelho inverte a ordem dos vertices: o x do recorte tambem e' negado
//    (o culling de face de tras continua certo) e o composite le' a coluna
//    espelhada de volta;
//  - o que esta' embaixo d'agua sairia na frente do reflexo: plano proximo
//    obliquo no nivel d'agua (obliqueNearPlane);
//  - o ModelRenderer troca o estado de LOD/histerese para o da vista
//    secundaria (beginSecondaryView) e escreve num trecho proprio do buffer de
//    instancias (cursor por quadro), como o minimapa.
// Medido (mapa de praia, 4090 sob xvfb, preset high, reflexo 640x360, 7 poses):
// 0,07-0,28 ms de GPU e ~0,1 M de triangulos. Antes do culling pelo plano
// d'agua e sem tirar do espelho o relevo assado e a tesselacao eram ~10 M de
// triangulos e 2,75 ms - quase um segundo G-buffer.
void Engine::renderWaterReflection(VkCommandBuffer cmd) {
    m_water.setMirrorReflectionSlot(-1);
    if (!m_waterReflectionOn || !m_currentMap) return;
    float level = 0.0f;
    if (!m_water.planarReflectionPlane(m_waterMenu.config, level)) return;
    if (!waterInViewThisFrame()) return;
    // Camera embaixo (ou no nivel) d'agua: nao ha' reflexo de cima para ver.
    if (m_camera.position().y <= level + 0.05f) return;
    const VkExtent2D ge = m_gbuffer.extent();
    if (!m_waterReflection.ensureSize(ge.width, ge.height, m_waterReflectionDiv) || !m_waterReflection.ready()) return;

    Mat4 mirror(1.0f);
    mirror[1][1] = -1.0f;
    mirror[3][1] = 2.0f * level;
    Mat4 flipX(1.0f);
    flipX[0][0] = -1.0f;
    const Vec4 waterPlane(0.0f, 1.0f, 0.0f, -level);
    Mat4 drawVp = flipX * m_camera.viewProjJittered() * mirror;
    obliqueNearPlane(drawVp, waterPlane);
    // Culling pela mesma matriz obliqua (sem o jitter): o plano proximo e' o
    // da agua, entao instancia e celula de chao inteiras embaixo d'agua saem
    // na CPU em vez de pagar o vertice para o recorte jogar fora.
    Mat4 cullVp = flipX * m_camera.viewProjNoJitter() * mirror;
    obliqueNearPlane(cullVp, waterPlane);
    Frustum frustum;
    frustum.extractFromMatrix(cullVp);
    // Camera espelhada em mundo: distancia de LOD e descarte por tela.
    const Vec3 cam = m_camera.position();
    const Vec3 mirroredCam(cam.x, 2.0f * level - cam.y, cam.z);

    const VkExtent2D re = m_waterReflection.extent();
    m_vulkan.writeTimestamp(cmd, 62, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    m_modelRenderer.beginSecondaryView();
    // Pixels por unidade NA RESOLUCAO DO REFLEXO: o LOD e o descarte de
    // objeto pequeno seguem o tamanho com que ele aparece aqui.
    m_modelRenderer.setScreenMetrics(
        static_cast<float>(re.height) / (2.0f * std::tan(glm::radians(m_camera.fov()) * 0.5f)),
        static_cast<float>(re.width), static_cast<float>(re.height));
    GBuffer& gb = m_waterReflection.gbuffer();
    gb.beginPass(cmd);
    m_terrainRenderer.render(cmd, drawVp, frustum);
    m_modelRenderer.render(cmd, drawVp, frustum, mirroredCam);
    drawWaterReflectionSprites(cmd, drawVp);
    gb.endPass(cmd);
    // ERUPTION_TEST_WATER_REFLECTION_LOG=1: o que o passe desenhou (uma linha a cada 60 quadros).
    static const bool kLog = std::getenv("ERUPTION_TEST_WATER_REFLECTION_LOG") != nullptr;
    if (kLog && m_framesCount % 60 == 0)
        ERUPTION_LOG_WARN("[REFLEXO] nivel %.2f visiveis %zu draws %u tris %u (%ux%u)", level,
                          m_modelRenderer.lastVisibleCount(), m_modelRenderer.lastDrawCalls(),
                          m_modelRenderer.lastDrawnTriangles(), re.width, re.height);
    m_modelRenderer.endSecondaryView();

    const LightingEnvironment& env = m_deferredLighting.environment();
    const VkImageView cloudView = renderWaterReflectionClouds(cmd, drawVp, flipX * m_camera.viewProjJittered() * mirror,
                                                              flipX * m_camera.viewProjNoJitter() * mirror, mirroredCam);
    WaterReflectionPass::Lighting light;
    light.toSun = -glm::normalize(env.sun.direction);
    light.sunIntensity = std::max(env.sun.intensity, 0.0f);
    light.sunColor = env.sun.color;
    light.directScale = m_deferredLighting.pbrLightScale() / 3.14159265f;
    light.ambientSky = env.ambientSkyColor;
    light.giIntensity = m_deferredLighting.giIntensity();
    light.ambientGround = env.ambientGroundColor;
    light.hemiFloor = m_deferredLighting.ambientHemiFloor();
    light.skyHorizon = env.skyHorizonColor;
    light.emissiveScale = 1.0f;
    m_waterReflection.composite(cmd, light, cloudView);
    m_vulkan.writeTimestamp(cmd, 63, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    Profiler::addGpuTime("Water Reflection", m_vulkan.timestampDeltaMs(62, 63));
    m_water.setMirrorReflectionSlot(static_cast<int32_t>(m_waterReflection.slot()));
}

} // namespace eruption
