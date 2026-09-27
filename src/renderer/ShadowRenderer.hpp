#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "math/Types.hpp"
#include "math/Camera.hpp"
#include "math/Frustum.hpp"
#include "utils/SplineCurve.hpp"

#include <vector>
#include <nlohmann/json.hpp>

namespace eruption {

using json = nlohmann::json;

class TerrainRenderer;
class ModelRenderer;
class SpriteRenderer;
struct FrameUBO;
struct DirectionalLight;

struct ShadowSettings {
    bool enabled = true;
    // "single" (default): UM mapa ortografico estavel cobrindo tudo que a
    // camera ve - zero costura de cascata, um texel, um bias, snap perfeito,
    // e o passe de sombra desenha METADE (slot 1 nem renderiza). Escolha
    // classica para camera isometrica/RTS de footprint limitado.
    // "cascade": caminho antigo c0+c1, mantido por compatibilidade/custom.
    // Override rapido p/ A-B: ERUPTION_SHADOW_MODE=single|cascade.
    bool singleMap = false;
    // "csm": cascatas ESTAVEIS (Valient, ShaderX6 "Stable Cascaded Shadow
    // Maps" + MJP): cada fatia do frustum e' envolvida por uma ESFERA - raio
    // depende SO' de fov/aspect/splits, nunca da pose da camera (rotacao E
    // translacao nao mudam o tamanho da caixa). O centro move continuo e o
    // snap de texel o trava na grade absoluta. E' o modo default; "single" e
    // "cascade" (ray-fit legado) continuam por compat/custom.
    bool csmStable = true;
    float csmFar = 1200.0f;     // alcance da ultima cascata (unidades de mundo)
    // Alcance ADAPTATIVO da ultima cascata: csmFar e' o PISO. Quando a camera
    // ve chao mais longe (zoom alto, pitch baixo) o alcance sobe ate csmFarMax,
    // SUAVIZADO no tempo (constante csmFarSmoothTau, segundos): o raio da
    // esfera - e o texel - anda continuamente, sem degrau. A versao anterior
    // quantizava em degraus de csmFarStep com histerese e o autor viu o
    // degrau como "snap de sombra no pitch" (2026-09-05).
    // Relato do autor (2026-09-05): "a sombra parece pequena, acompanha a
    // camera e some de onde ta o frustum" - alem da c1 o shader devolve LUZ
    // (directional.frag: return 0.0), e com sf fixo em 1200 o chao visivel a
    // 2-3 mil unidades ficava sem sombra nenhuma. ERUPTION_SHADOW_FAR_ADAPTIVE=0
    // desliga p/ A-B.
    bool csmFarAdaptive = true;
    float csmFarMax = 3500.0f;
    float csmFarStep = 250.0f;      // legado (sem leitor desde a suavizacao)
    float csmFarSmoothTau = 0.25f;  // s; ERUPTION_SHADOW_FAR_TAU
    float csmLambda = 0.6f;     // practical split: 0=uniforme, 1=logaritmico
    // Exigencia de texels na cascata DISTANTE (multiplica min_shadow_texels
    // so' na c1+). Um prop que cobre 1-2 texels de uma cascata larga nao
    // desenha sombra reconhecivel, mas paga submissao de caster - o item
    // mais caro do csm. 2.5 = precisa cobrir ~2.5 texels pra entrar.
    float csmFarTexelBias = 2.5f;
    // Orcamento de texel do mapa unico (unidades de mundo por texel). Define o
    // alcance da sombra: qh_max = texel * atlas/2 (0.5 * 4096/2 = 1024u).
    // Perto = denso e estavel; alem do alcance = sem sombra (fade no shader).
    float singleMaxTexel = 0.5f;
    // Per-cascade atlas resolution (atlas is 2*size x size, D32). 4096 was
    // hardcoded before; weak GPUs (930M profile) should run 2048 or lower via
    // "atlas_size" in data/shadows.json.
    int atlasSize = 4096;
    bool usePoisson = false;
    int poissonTaps = 4;
    int pcfKernelSize = 7;
    float cascadeRadius[4] = {1000.0f, 500.0f, 2000.0f, 10000.0f};
    bool debugLightRays = false;
    bool debugMagentaShadow = false;    // paints sun-shadowed fragments magenta (F2 checkbox)

    // Core Stability
    // NOTE: enableAngleSnapping + enableTemporalBlend are DEPRECATED.
    // They cause visible ghosting/blocking artifacts. Use enablePositionalSnap instead.
    bool enableTemporalBlend = false;
    bool enableAngleSnapping = false;
    bool enableShadowCulling = true;
    float biasValue = 0.0005f;         // Constant shadow-map depth bias
    float normalBias = 0.005f;         // Normal-offset bias to fight acne on flat planes
    float slopeBias = 8.0f;            // Fator do bias escalado por tan(angulo) (acne em encosta)
    
    // Positional Snap: snaps shadowCenter to a fixed world-space grid.
    // This eliminates shimmering WITHOUT the blocky artifacts of texel-projection snap.
    bool enablePositionalSnap = true;
    float snapQuantizeStep = 50.0f;    // Quantize halfSize to multiples of this (meters)
    
    SplineCurve c0SizeCurve;
    // Tamanho do mapa unico (modo singleMap), so' funcao de ZOOM - nunca de
    // angulo/rotacao/posicao da camera. Default embutido (funciona sem
    // "single_size_curve" no JSON); zoomPercent=1 e' zoom MAXIMO (perto).
    SplineCurve singleSizeCurve = SplineCurve({0.0f, 0.5f, 1.0f}, {2500.0f, 1200.0f, 500.0f},
                                              InterpolationMode::Smoothstep);
    SplineCurve minModelShadowSizeCurve;
    SplineCurve farRangeCurve; 
    
    bool enableAdaptiveShadows = true;

    // Low-pitch coverage boost: at grazing camera pitches (~10-40 deg) the
    // view stretches to the horizon, but the ortho shadow box is sized by
    // zoom only — distant buildings fall outside the box (the lighting
    // shader returns "no shadow" outside) and the shadow edge visibly
    // sweeps along as the player walks. halfSize is multiplied by
    // mix(lowPitchSizeBoost, 1, smoothstep(0, 0.45, tPitch)).
    float lowPitchSizeBoost = 3.0f;
    // Cap for the boost: never let the boosted world texel exceed this
    // (meters). At far zoom the c0 box is already kilometers wide (coarse
    // texels); boosting it further would only make the positional snap jump
    // in huge steps without adding useful coverage. The boost applies in
    // full exactly where the artifact was: close/mid zoom at low pitch.
    float lowPitchMaxTexel = 6.0f;

    // Piso FÍSICO do descarte de caster de sombra, em texels do shadow map.
    // A curva por zoom (minModelShadowSizeCurve) descartava caster com raio
    // menor que 10 unidades no zoom-out total, o que apagava a sombra de quase
    // todo prop - mesmo quando o shadow map tinha resolução de sobra para ela
    // (no zoom-out a cascata 0 fica com ~0.22 unidade por texel, ou seja um
    // prop de raio 2 ainda ocupa ~18 texels). Agora o descarte só vale para
    // caster cuja sombra é realmente menor que este número de texels, isto é,
    // impossível de resolver. Regra do projeto: se tem sombra, mantenha.
    // Subir para 2-3 é a alavanca de performance para hardware fraco.
    float minShadowTexels = 1.0f;
    
    void loadFromJson(const json& j);
private:
    static SplineCurve loadSplineFromJson(const json& node);
};

class ShadowRenderer {
public:
    bool init(VulkanContext* ctx, BindlessDescriptor* bindless);
    void shutdown();

    void resizeAtlas(uint32_t newSize);
    void setShadowSnaps(const float* yawSnaps, const float* timeSnaps, uint32_t count);
    void updateCascades(const DirectionalLight& light, const Camera& camera, float zoomPercent, float timeOfDay);
    void renderCascades(VkCommandBuffer cmd, TerrainRenderer* terrain, ModelRenderer* models, SpriteRenderer* sprites, const FrameUBO& frameUbo, VkBuffer spriteInstanceBuffer, uint32_t spriteCount);

    // Accessors
    VkImageView shadowAtlasView() const { return m_atlasView; }
    // Placeholder = atlas principal quando o "next" nao existe (temporal
    // blend desligado): o descriptor precisa de view valida, o shader nunca
    // a amostra com shadowRanges.w == 0.
    VkImageView nextShadowAtlasView() const {
        return m_nextAtlasView != VK_NULL_HANDLE ? m_nextAtlasView : m_atlasView;
    }
    VkSampler shadowSampler() const { return m_shadowSampler; }
    const Mat4* getCascadeMatrices() const { return m_cascadeLookupMatrices; }
    const Mat4* getNextCascadeMatrices() const { return m_nextCascadeLookupMatrices; }
    const Vec4& getCascadeSplits() const { return m_cascadeSplitsVec; }
    float getBlendFactor() const { return m_blendFactor; }
    float getWorldTexelSize() const { return m_currentWorldTexelSize; }
    float getC1WorldTexelSize() const { return m_c1WorldTexelSize; }
    float getShadowDepthRange() const { return m_shadowDepthRange; }
    Vec3 currentLightDir() const { return m_currentLightDir; }

    VkPipeline shadowPipeline() const { return m_shadowPipeline; }
    // Variante INSTANCIADA (shadow_inst.vert): VK_NULL_HANDLE se nao existir.
    VkPipeline shadowInstPipeline() const { return m_shadowInstPipeline; }
    VkPipeline shadowInstOpaquePipeline() const { return m_shadowInstOpaquePipeline; }
    VkPipelineLayout shadowInstLayout() const { return m_shadowInstLayout; }
    VkPipelineLayout shadowLayout() const { return m_shadowLayout; }

    ShadowSettings& settings() { return m_settings; }
    const ShadowSettings& settings() const { return m_settings; }

    // c0 = legacy square box (near shadows, unchanged); c1 = rectangular
    // frustum-fit box extending coverage to the fog. The shader samples c0
    // wherever its box contains the fragment, c1 outside it.
    static constexpr uint32_t CASCADE_COUNT = 2;
    static constexpr uint32_t MIN_ATLAS_SIZE = 512;
    static constexpr uint32_t MAX_ATLAS_SIZE = 16384;
    
    static constexpr float TARGET_TEXEL_DENSITY = 16.0f;
    static constexpr uint32_t MAX_SHADOW_SNAPS = 16;
    uint32_t m_shadowSnapCount = 0;
    float m_shadowYawSnaps[MAX_SHADOW_SNAPS] = {0};
    float m_shadowTimeSnaps[MAX_SHADOW_SNAPS] = {0};

private:
    VulkanContext* m_ctx = nullptr;
    BindlessDescriptor* m_bindless = nullptr;

    VkImage m_atlasImage = VK_NULL_HANDLE;
    VmaAllocation m_atlasAlloc = VK_NULL_HANDLE;
    VkImageView m_atlasView = VK_NULL_HANDLE;
    
    VkImage m_nextAtlasImage = VK_NULL_HANDLE;
    VmaAllocation m_nextAtlasAlloc = VK_NULL_HANDLE;
    VkImageView m_nextAtlasView = VK_NULL_HANDLE;
    
    VkSampler m_shadowSampler = VK_NULL_HANDLE;

    // CSM data
    Mat4 m_cascadeRenderMatrices[CASCADE_COUNT];
    Mat4 m_cascadeLookupMatrices[CASCADE_COUNT];
    Mat4 m_nextCascadeRenderMatrices[CASCADE_COUNT];
    Mat4 m_nextCascadeLookupMatrices[CASCADE_COUNT];
    float m_cascadeSplits[CASCADE_COUNT];
    Vec4 m_cascadeSplitsVec;
    float m_blendFactor = 0.0f;
    float m_zoomPercent = 1.0f;
    float m_currentWorldTexelSize = 0.01f;
    float m_c1WorldTexelSize = 0.01f;
    // ESTADO DO MODO CASCADE LEGADO (m_settings.singleMap == false) - o modo
    // default (single) nao usa nenhum destes: o ray-grid-fit por angulo que
    // exigia essa historia de congelamento foi removido do caminho single,
    // nao ajustado. Meia-extensao estavel da cascata 1 (histerese) + chave de
    // vista em que foi calculada (o tamanho so' depende de angulo/zoom, nunca
    // de posicao - andando reto ela congela). Ver ShadowRenderer.cpp.
    float m_c1StableHalf = 0.0f;
    float m_c1KeyYaw = 1e9f;
    float m_c1KeyPitch = 1e9f;
    float m_c1KeyZoom = 1e9f;
    float m_shadowDepthRange = 15000.0f;
    Vec3 m_currentLightDir = Vec3(0.0f, -1.0f, 0.0f);

    // Temporal stabilization
    float m_currentHalfSize = 0.0f;
    // Alcance/near efetivos da ultima cascata no modo csm (suavizados no
    // tempo) - ver ShadowSettings::csmFarAdaptive.
    float m_csmFarCurrent = 0.0f;
    float m_csmNearCurrent = 0.0f;
    double m_csmSmoothLastTime = -1.0; // relogio proprio: updateCascades nao recebe dt

    // Shadow pipeline
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
    // Pipeline INSTANCIADO, so' para modelos (shadow_inst.vert): matriz por
    // caster vem de SSBO (set 1) e cada malha vira um draw por cascata.
    // Terreno/chuva continuam no m_shadowPipeline com mvp por push.
    VkPipeline m_shadowInstPipeline = VK_NULL_HANDLE;
    // Variante sem fragment shader, para caster cujas texturas nao tem
    // alfa. Ver o comentario na criacao, em ShadowRenderer.cpp.
    VkPipeline m_shadowInstOpaquePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_shadowInstLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_shadowInstSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_shadowLayout = VK_NULL_HANDLE;

    ShadowSettings m_settings;
    uint32_t m_currentAtlasSize = 4096;

    void createShadowAtlas(uint32_t size);
    bool createNextAtlasImage(uint32_t size);
    void ensureNextAtlas();
    void createShadowSampler();
    void createShadowPipeline();
};

} // namespace eruption
