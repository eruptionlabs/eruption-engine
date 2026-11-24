#pragma once
#include "renderer/PushRing.hpp"

#include "renderer/VulkanContext.hpp"
#include "renderer/GBuffer.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "math/Types.hpp"
#include "math/Frustum.hpp"

#include <cstdlib>
#include <vector>

namespace eruption {

// Forward declarations
class ShadowRenderer;
class Camera;

// Luz presa ao sprite principal (F10): vive na lista das luzes do mapa e é
// reposta no fim dela a cada quadro (Render.cpp). Marcada por este valor em
// shadowCubemapIndex (o shader não lê o campo). Não é luz do mapa: os
// objetos da cena e o editor a deixam de fora.
constexpr uint32_t kPlayerLightTag = 0xFFFFFFFEu;

struct PointLight {
    Vec3 position = Vec3(0.0f);
    float radius = 10.0f;
    Vec3 color = Vec3(1.0f);
    float intensity = 1.0f;
    // Intensidade sem tremulacao (luz de efeito): a forca da sombra de sprite
    // usa esta quando >= 0, para a sombra nao piscar com a tocha.
    float steadyIntensity = -1.0f;
    LightAnimType animType = LightAnimType::Static;
    uint32_t shadowCubemapIndex = 0xFFFFFFFF;
    bool enabled = true;
    // When true, Engine toggles `enabled` off/on by time of day instead of
    // leaving it always on (lamp/lantern props: lit only at night).
    bool nightOnly = false;
    // Horario proprio de uma luz noturna (hora do dia 0..24, negativo = pela
    // curva do sol) e cintilacao: intensidade*(1 - amp*ruido) a flickerSpeed Hz.
    // A intensidade de autoria fica em `intensity`; o motor escreve o fator do
    // quadro em `flickerMul` (Engine::updateNightLights) e o desenho multiplica.
    float onHour = -1.0f, offHour = -1.0f;
    float flickerAmp = 0.0f, flickerSpeed = 6.0f;
    float flickerMul = 1.0f;
    // Brilho do objeto: superficies a ate' glowRadius da luz (unidades) ganham a
    // cor dela vezes glowStrength (model_frag.glsl, FrameUBO::glow*). 0 = nao.
    float glowRadius = 0.0f, glowStrength = 0.0f;
    // Spot: direcao (normalizada) e cosseno da metade da abertura. Direcao
    // zero = luz pontual (o padrao). Luz de janela cai para fora e para baixo.
    Vec3 spotDir = Vec3(0.0f);
    float spotCos = 0.0f;
    // Projeta sombra de sprite (Engine::collectSpritePointShadows). O jogo
    // desliga na luz que ele move junto com o grupo (lights.set_sprite_shadow).
    bool spriteShadow = true;
    // Solta raios no passe de god rays (PostProcessor::renderGodRays). Luz de
    // efeito so' com "godRays": true no .vfx.json.
    bool godRays = true;
};

struct ShadowUBO {
    Mat4 lightSpaceMats[2];
    Mat4 nextLightSpaceMats[2];
    Vec4 cascadeSplits;
    // x = c0 world texel size, y = c1 world texel size (used for
    // texel-scaled depth bias), z = magenta debug tint opacity,
    // w = 1 forces sun shadow factor to 0 (debug A/B).
    Vec4 cascadeScales;
    Vec4 shadowParams;     // x = pcfKernelSize, y = poissonTaps, z = usePoisson, w = time
    Vec4 shadowRanges;     // x = blendRange, y = depthRange (real ortho near..far), z = shadowBias, w = blendFactor
    // OPACIDADE DA SOMBRA POR DISTANCIA, como LUT de 8 amostras uniformes em
    // [0, shadowOpacityParams.x]. y = 1 liga / 0 desliga. 1 = preto cheio.
    Vec4 shadowOpacityParams;
    Vec4 shadowOpacityLut0;
    Vec4 shadowOpacityLut1;
};

// Caminho enxuto: luzes de ponto por bloco da tela (ver fillLightTiles).
constexpr uint32_t kLightTilesX = 16, kLightTilesY = 8, kLightTiles = kLightTilesX * kLightTilesY;

struct LightingUBO {
    Vec4 dirLightDir;       // xyz=dir, w=intensidade
    Vec4 dirLightColor;     // xyz=cor RGB, w=ambientIntensity
    Vec4 pointLights[128];  // xyz=posição, w=raio
    Vec4 pointColors[128];  // xyz=cor, w=intensidade final (já modulada)
    uint32_t numPointLights;
    float giIntensity;
    float giAmbientFloor;
    uint32_t usePbr;        // 0 = legacy Blinn-Phong, 1 = Cook-Torrance/GGX
    uint32_t pbrDebugMode;  // bitmask: 1=roughness, 2=metallic, 4=normal, 8=wetness, 16=source
    float rainIntensity;    // 0..1 global rain intensity (wettable/water surfaces get wet)
    float snowIntensity;    // 0..1 global snow intensity (frost/snow surfaces freeze/accumulate)
    float temperatureC;     // surface temperature in Celsius
    float pbrLightScale;    // multiplier for PBR direct-light response (legacy stays unchanged)
    // Raio angular da fonte de luz, em radianos (era `pad0`, provado sem
    // leitor no commit 2290b17). O sol subtende ~0,53 grau = 0,0046 rad; um
    // softbox de 60 cm a 40 cm de uma maquete subtende ~37 graus. E' este
    // numero que decide se a penumbra le' como exterior 1:1 ou como foto de
    // maquete - ate' agora o raio de PCF era FIXO, o que equivale a dizer
    // "fonte pontual no infinito" em toda cena.
    float sunAngularRadius;
    // Luz de borda dos sprites do lado do sol (directional.frag): unorm 4x8,
    // rgb = tinta, a = forca / 2. 0 = desligada (era pad1).
    uint32_t spriteRim;
    alignas(16) Vec4 ambientSky;        // xyz=skyColor, w=unused
    alignas(16) Vec4 ambientGround;     // xyz=groundColor, w=unused
    alignas(16) Vec4 ssaoParams;        // x=enabled(0/1), y=strength, z=radius, w=raio da escala curta (0 = so' a larga)
    alignas(16) Vec4 indirectParams;   // x=envSpecEnabled, y=envSpecIntensity, z=nightAoPow, w=ambientHemiFloor
    alignas(16) Mat4 viewProj;         // camera view-projection (SSAO reprojection, contact shadows)
    alignas(16) Vec4 bounceParams;     // x=sunBounceEnabled, y=sunBounceStrength, z=fillLightIntensity, w=unused
    alignas(16) Vec4 contactParams;    // x=contactShadowEnabled, y=lengthMetres, zw=unused
    alignas(16) Vec4 skyHorizon;       // xyz=procedural sky horizon colour (IBL env eval)
    // ACRESCENTADO NO FIM de proposito: os offsets de todos os campos acima
    // ficam intactos, entao shaders que truncam a struct continuam validos.
    // Necessario desde que o G-buffer deixou de carregar WorldPos e os passes
    // reconstroem a posicao a partir do depth.
    alignas(16) Mat4 invViewProj;
    // SONDA DE CEU (G36): x=ligada, y=mip maximo do cubemap (roughness->lod),
    // z=intensidade, w=livre. Tambem no fim, pelo mesmo motivo.
    alignas(16) Vec4 skyProbeParams;
    // PROBES DE IRRADIANCIA (G38): xyz = canto minimo da grade, w = ligadas;
    // xyz = 1/extensao da grade, w = forca (0..1).
    alignas(16) Vec4 probeGridMin;
    alignas(16) Vec4 probeGridInvExtent;
    // Caminho enxuto: ate' 16 indices de luz de ponto (8 bits, 0xFF fecha a
    // lista) por bloco de kLightTilesX x kLightTilesY da tela. No fim, como
    // os dois acima.
    alignas(16) glm::uvec4 tileLights[kLightTiles];
    // CHUVA LOCAL: o chao so' molha debaixo das nuvens que chovem. Ate'
    // kRainSpots discos (xy = centro XZ, z = raio); rainSpotParams.x = quantos,
    // y = 1 se ha' campo de nuvens (sem ele a chuva global molha tudo). No fim.
    alignas(16) Vec4 rainSpots[64];
    alignas(16) Vec4 rainSpotParams;
    // BEIRA D'AGUA: grade de "perto de agua visivel" (binding 18). xy = canto
    // XZ, z = 1/lado em u (0 = sem grade: nem molhado de onda nem lamina),
    // w = lamina: 0 desligada, 1 sem espuma, 2 com espuma na frente. No fim.
    alignas(16) Vec4 shoreMask;
    alignas(16) Vec4 shoreWaterColor; // rgb = cor funda da agua, w = clareza (u ate' o vermelho cair a 1/e)
    // Por liquido da grade da beira (G da grade: 0 = agua do mapa, 1.. = lavas):
    // x = nivel (< -1e8 = sem), y = modo (0 contato, 1 invade, 2 invade com
    // espuma), z = tipo (0 agua, 1 lava), w = brilho da lava. No fim.
    alignas(16) Vec4 shoreLiquids[8];
    // CHUVA SO' ONDE ELA CHEGA (binding 19, a grade de cima da chuva do
    // WeatherRenderer: a altura do que esta' mais alto em cada ponto): xy =
    // canto XZ, z = 1/lado (0 = sem grade: molha tudo), w = borda suave (u).
    // Telhado, marquise e copa ficam secos por baixo. No fim.
    alignas(16) Vec4 rainCover;
    // MOLHADO LOCAL (world.wet_area: skill de agua, balde...): xy = centro XZ,
    // z = raio, w = quanto (0..1, ja' secando). wetAreaParams.x = quantas.
    alignas(16) Vec4 wetAreas[32];
    alignas(16) Vec4 wetAreaParams;
};

struct DirectionalLight {
    Vec3 direction = Vec3(0.0f, -1.0f, 0.0f);
    Vec3 color = Vec3(1.0f);
    float intensity = 1.0f;
    Mat4 lightSpaceMatrices[4];
    float cascadeSplits[4];
    uint32_t shadowMapIndex = 0;
};

struct SpotLight {
    Vec3 position = Vec3(0.0f);
    Vec3 direction = Vec3(0.0f, -1.0f, 0.0f);
    Vec3 color = Vec3(1.0f);
    float intensity = 1.0f;
    float range = 10.0f;
    float innerCone = 0.9f;  // cos(angle)
    float outerCone = 0.8f;  // cos(angle)
    uint32_t shadowMapIndex = 0xFFFFFFFF;
};

struct LightingEnvironment {
    DirectionalLight sun;
    Vec3 ambientColor = Vec3(0.1f, 0.1f, 0.15f);
    float ambientIntensity = 0.7f;
    Vec3 ambientSkyColor = Vec3(0.4f, 0.5f, 0.7f);    // Zenite / topo do sky
    Vec3 ambientGroundColor = Vec3(0.1f, 0.08f, 0.05f); // Albedo médio do terreno
    Vec3 skyHorizonColor = Vec3(0.6f, 0.65f, 0.75f);    // Horizonte do céu procedural
};

class DeferredLighting {
public:
    bool init(VulkanContext* ctx, GBuffer* gbuffer, BindlessDescriptor* bindless,
              uint32_t width, uint32_t height);
    void shutdown();
    void resize(uint32_t width, uint32_t height);

    void setEnvironment(const LightingEnvironment& env);
    const LightingEnvironment& environment() const { return m_env; }
    void setGlobalIllumination(float intensity, float floor) { m_giIntensity = intensity; m_giAmbientFloor = floor; }
    float giIntensity() const { return m_giIntensity; }
    float ambientHemiFloor() const { return m_ambientHemiFloor; }
    void setSunAngularRadius(float radians) { m_sunAngularRadius = radians; }
    // Luz de borda dos sprites do lado do sol: tinta (0..1) e forca (0..2; 0 desliga).
    void setSpriteRim(const Vec3& tint, float strength);
    // O raio em uso (com o override de teste): a busca de bloqueador do PCSS
    // depende dele, e o recorte de casters da sombra precisa do mesmo alcance.
    float sunAngularRadius() const;
    void setPointLights(const std::vector<PointLight>& lights);
    // Gate do passe inteiro. Antes NADA lia o RenderEffect "Point Lights" nem
    // a chave "point_lights" do preset (o `low` dizia false e as luzes
    // desenhavam do mesmo jeito) - o mesmo defeito da caixa de bloom.
    void setPointLightsEnabled(bool on) { m_pointLightsEnabled = on; }
    // Teto de luzes de ponto por quadro (0 = todas; no caminho enxuto, ate' as
    // 128 do UBO): ficam as mais perto do alvo da camera. Preset "point_light_max".
    void setPointLightMax(uint32_t n) { m_pointLightMax = n; }
    // Caminho enxuto: luzes por bloco da tela, 1..16 (preset "point_light_tile_max").
    void setTileLightMax(uint32_t n) { m_tileLightMax = std::clamp(n, 1u, 16u); }
    // SONDA DE CEU (G36): cubemap pre-filtrado (binding 15) + SH9 (binding 16).
    // Escreve os descritores de TODOS os frames uma vez (as views sao fixas) -
    // chamar no init, antes do primeiro frame, para nao esbarrar em set
    // pendente (o binding 16 nao tem UPDATE_AFTER_BIND).
    void setSkyProbe(VkImageView cubeView, VkSampler sampler, VkBuffer shBuffer,
                     VkDeviceSize shSize, uint32_t mipCount);
    void setSkyProbeParams(bool enabled, float intensity) {
        m_skyProbeEnabled = enabled;
        m_skyProbeIntensity = intensity;
    }
    // PROBES DE IRRADIANCIA (G38): textura 3D (binding 17, UPDATE_AFTER_BIND -
    // pode ser trocada no meio do jogo, ao carregar mapa). `enabled` = grade
    // real (false = fallback 1x1x1 "ceu aberto", a leitura vira identidade).
    void setIrradianceProbes(VkImageView view, VkSampler sampler,
                             const Vec3& gridMin, const Vec3& gridInvExtent, bool enabled);
    void setIrradianceProbeStrength(float strength) { m_probeStrength = std::clamp(strength, 0.0f, 1.0f); }
    std::vector<PointLight>& getPointLights() { return m_pointLights; }
    const std::vector<PointLight>& getPointLights() const { return m_pointLights; }
    // Lista do quadro (mapa + extras + VFX) montada pelo passe de luz; o pos
    // le' depois dele (raios de luz das luzes pontuais).
    // ShadowUBO deste quadro (matrizes do mapa de sombra do sol), para quem
    // consulta a sombra do sol fora da iluminacao (disco do "map_contact").
    VkBuffer shadowUboBuffer() const { return m_shadowUboBuffers[m_ctx->currentFrame()]; }
    const std::vector<PointLight>& frameLights() const {
        return (m_extraPointLights.empty() && m_vfxPointLights.empty()) ? m_pointLights : m_allPointLights;
    }
    // Luzes dos mapas vizinhos (mundo contínuo): somadas às do mapa aberto,
    // fora da lista que a cena e o editor numeram.
    void setExtraPointLights(std::vector<PointLight> lights) { m_extraPointLights = std::move(lights); }
    // Luzes dos efeitos de particula (trocadas todo frame, fora da cena).
    void setVfxPointLights(const std::vector<PointLight>& lights) { m_vfxPointLights = lights; }
    const std::vector<PointLight>& vfxPointLights() const { return m_vfxPointLights; }

    void setUsePbr(bool use) { m_usePbr = use; }
    bool usePbr() const { return m_usePbr; }
    void setPbrDebugMode(uint32_t mode) { m_pbrDebugMode = mode; }
    uint32_t pbrDebugMode() const { return m_pbrDebugMode; }
    void setPbrLightScale(float scale) { m_pbrLightScale = scale; }
    float pbrLightScale() const { return m_pbrLightScale; }

    // Indirect-lighting controls (inline contact SSAO + analytic env specular +
    // ambient shaping). Driven by the Lighting tab.
    // Direcoes do kernel do SSAO (4..12); refaz o pipeline se mudar.
    void setSsaoTaps(int taps);
    // Segunda escala do SSAO, curta (chaves "ssao_radius_short"/"ssao_taps_short"):
    // raio em u (0 desliga) e direcoes (0 = sem o laco; refaz o pipeline se mudar).
    void setSsaoShort(float radius, int taps);
    // SSAO em resolucao cheia (chave "ssao_full_res"; padrao meia resolucao): o raio
    // curto enxerga as frestas da tesselacao, que a meia resolucao dilui. Refaz os
    // alvos do SSAO e o pipeline se mudar.
    void setSsaoFullRes(bool fullRes);
    void setIndirectParams(bool ssao, float ssaoStrength, float ssaoRadius,
                           bool envSpec, float envSpecIntensity,
                           float nightAoPow, float ambientHemiFloor) {
        m_ssaoEnabled = ssao; m_ssaoStrength = ssaoStrength; m_ssaoRadius = ssaoRadius;
        m_envSpecEnabled = envSpec; m_envSpecIntensity = envSpecIntensity;
        m_nightAoPow = nightAoPow; m_ambientHemiFloor = ambientHemiFloor;
    }
    void setBounceParams(bool enabled, float strength) {
        m_sunBounceEnabled = enabled; m_sunBounceStrength = strength;
    }
    // Luz de preenchimento de direção fixa (sem sombra): mantém o relevo do
    // normal map legível onde o sol não bate ("fake 3D" na sombra).
    void setShadowSoft(int taps) { m_shadowSoft = static_cast<float>(taps); }
    void setFillLight(float intensity) { m_fillLightIntensity = intensity; }
    // Microsombra (pbr_common.glsl): forca 0..1 e expoente do AO.
    void setMicroShadow(float strength, float contrast, float floorV) {
        m_microShadow = strength; m_microShadowContrast = contrast; m_microShadowFloor = floorV;
    }
    // Curva de opacidade da sombra (ver ShadowUBO::shadowOpacityKnots).
    void setShadowOpacityCurve(const Vec4& params, const Vec4& lut0, const Vec4& lut1) {
        m_shadowOpacityParams = params;
        m_shadowOpacityLut0 = lut0;
        m_shadowOpacityLut1 = lut1;
    }
    void setContactShadowParams(bool enabled, float lengthMetres) {
        m_contactShadowEnabled = enabled; m_contactShadowLength = lengthMetres;
    }

    // Climate state drives rain wetting, snow/frost accumulation and surface
    // temperature for the PBR semantic material system.
    // Discos de chuva (centro XZ, raio) das nuvens que chovem; local = false:
    // nao ha' campo de nuvens e a chuva global vale no mapa inteiro.
    void setRainSpots(const std::vector<Vec4>& spots, bool local) {
        m_rainSpotCount = static_cast<uint32_t>(std::min<size_t>(spots.size(), 64));
        for (uint32_t i = 0; i < m_rainSpotCount; ++i) m_rainSpots[i] = spots[i];
        m_rainSpotsLocal = local;
    }
    // Beira d'agua molhada pela onda: nivel da agua (sem agua: has = false) e o tempo.
    // Grade R8 (1 = agua visivel, cai a 0 longe da margem) e a chave da
    // lamina. Sem grade (invSize 0) a beira d'agua nao molha nada.
    // Grade da beira: 4 floats por celula (proximidade, liquido, altura da
    // superficie dele, livre), sobe como RGBA16F.
    void setShoreMask(const std::vector<float>& px, uint32_t w, uint32_t h, const Vec2& origin, float invSize);
    // 0 = sem lamina, 1 = lamina, 2 = lamina com espuma na frente.
    // A lamina absorve como o corpo d'agua (mesma cor funda e clareza): na
    // linha d'agua o tom da areia coberta emenda com o do fundo visto pela agua.
    void setShoreSwash(int mode, const Vec3& deepColor, float clarity) {
        m_shoreSwashMode = mode; m_shoreWaterColor = deepColor; m_shoreClarity = clarity;
    }
    static constexpr uint32_t kShoreLiquids = 8;
    void setShoreLiquid(uint32_t slot, const Vec4& v) { if (slot < kShoreLiquids) m_shoreLiquids[slot] = v; }
    void setShoreWater(bool has, float waterY, float time) {
        m_shoreWaterY = has ? waterY : -1.0e9f;
        m_shoreTime = time;
    }
    // Grade da cobertura da chuva (R32_UINT, a do WeatherRenderer) e onde ela
    // fica: canto XZ, lado, borda suave. view nula ou lado 0: sem grade (a
    // chuva molha tudo, como antes).
    void setRainCover(VkImageView view, const Vec2& origin, float size, float softness);
    static constexpr uint32_t kWetAreas = 32;
    void setWetAreas(const std::vector<Vec4>& areas) {
        m_wetAreaCount = static_cast<uint32_t>(std::min<size_t>(areas.size(), kWetAreas));
        for (uint32_t i = 0; i < m_wetAreaCount; ++i) m_wetAreas[i] = areas[i];
    }
    void setWeatherState(float rainIntensity, float snowIntensity, float temperatureC) {
        m_rainIntensity = rainIntensity;
        m_snowIntensity = snowIntensity;
        m_temperatureC = temperatureC;
    }

    void render(VkCommandBuffer cmd, const Camera& camera, const ShadowRenderer* shadows,
                VkImageView cloudShadowView, VkSampler cloudShadowSampler,
                const Vec3& cloudWorldMin, const Vec3& cloudWorldMax,
                float cloudShadowIntensity,
                VkImageView rainOcclusionView, VkSampler rainOcclusionSampler,
                float totalTime);

    VkImageView litImageView() const { return m_litView; }
    VkImage litImage() const { return m_litImage; }

private:
    // Ligações feitas depois do init (sonda do céu, sondas de irradiância):
    // o resize realoca os descritores e as reaplica a partir daqui.
    struct SkyProbeBinding { VkImageView view = VK_NULL_HANDLE; VkSampler sampler = VK_NULL_HANDLE;
                             VkBuffer sh = VK_NULL_HANDLE; VkDeviceSize shSize = 0; uint32_t mips = 0; };
    SkyProbeBinding m_skyProbeBinding;
    struct ProbeGridBinding { VkImageView view = VK_NULL_HANDLE; VkSampler sampler = VK_NULL_HANDLE;
                              Vec3 min{0.0f}, invExtent{0.0f}; bool enabled = false; };
    ProbeGridBinding m_probeGridBinding;
    VulkanContext* m_ctx = nullptr;
    GBuffer* m_gbuffer = nullptr;
    BindlessDescriptor* m_bindless = nullptr;

    // HDR output
    VkImage m_litImage = VK_NULL_HANDLE;
    VmaAllocation m_litAlloc = VK_NULL_HANDLE;
    VkImageView m_litView = VK_NULL_HANDLE;

    // Light volumes
    VkBuffer m_sphereVertexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_sphereVertexAlloc = VK_NULL_HANDLE;
    uint32_t m_sphereVertexCount = 0;
    VkBuffer m_sphereIndexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_sphereIndexAlloc = VK_NULL_HANDLE;
    uint32_t m_sphereIndexCount = 0;

    // Pipelines
    VkPipeline m_directionalPipeline = VK_NULL_HANDLE;
    VkPipeline m_pointLightPipeline = VK_NULL_HANDLE;
    VkPipeline m_ambientPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

    // Descriptor set layout for G-Buffer + shadow sampling
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;

    static constexpr uint32_t MAX_FRAMES = 3;
    VkDescriptorSet m_descSets[MAX_FRAMES] = {};

    // G-Buffer sampling
    VkSampler m_gbufferSampler = VK_NULL_HANDLE;

    // Shadow UBO (Cascades + settings)
    VkBuffer m_shadowUboBuffers[MAX_FRAMES] = {};
    VmaAllocation m_shadowUboAllocs[MAX_FRAMES] = {};
    void* m_shadowUboMappeds[MAX_FRAMES] = {};

    // Lighting UBO (Directional + Point Lights)
    VkBuffer m_lightingUboBuffers[MAX_FRAMES] = {};
    VmaAllocation m_lightingUboAllocs[MAX_FRAMES] = {};
    void* m_lightingUboMappeds[MAX_FRAMES] = {};
    
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    LightingEnvironment m_env;
    std::vector<PointLight> m_pointLights;
    // Blocos por draw da luz (ate' 256 B): set 1 do layout via PushRing,
    // nao push constant (GPU de celular so' aceita 128 B).
    PushRing m_pushRing;
    std::vector<PointLight> m_extraPointLights;
    std::vector<PointLight> m_vfxPointLights;
    std::vector<PointLight> m_allPointLights; // m_pointLights + m_extraPointLights, montado no frame
    const std::vector<PointLight>& allPointLights();
    bool m_pointLightsEnabled = true;
    uint32_t m_pointLightMax = 0;
    uint32_t m_tileLightMax = 16;
    std::vector<std::pair<float, uint32_t>> m_pointLightPick; // (distancia^2, indice) para o teto
    bool m_skyProbeEnabled = false;
    bool m_skyProbeBound = false;
    float m_skyProbeIntensity = 1.0f;
    float m_skyProbeMaxMip = 0.0f;
    bool m_probesEnabled = false;
    float m_probeStrength = 1.0f;
    Vec3 m_probeGridMin = Vec3(0.0f);
    Vec3 m_probeGridInvExtent = Vec3(0.0f);
    float m_solarIntensity = 1.0f; // Track solar intensity for artificial mod
    // Raio angular da fonte de sol, em radianos. Default = disco solar real
    // (0,53 grau). O preset diorama sobe isso pra fingir softbox.
    float m_sunAngularRadius = 0.0046f;
    uint32_t m_spriteRim = 0; // LightingUBO::spriteRim
    float m_giIntensity = 1.0f;
    float m_giAmbientFloor = 0.10f;
    bool m_usePbr = [] { return std::getenv("ERUPTION_TEST_NO_PBR") == nullptr; }();
    Vec4 m_shadowOpacityParams{1000.0f, 0.0f, 0.0f, 0.0f};
    float m_shadowSoft = 0.0f; // 0/1/2: PCF da variante leve (shadowOpacityParams.z no shader)
    Vec4 m_shadowOpacityLut0{1.0f, 1.0f, 1.0f, 1.0f};
    Vec4 m_shadowOpacityLut1{1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t m_pbrDebugMode = 0;
    float m_pbrLightScale = 2.0f;
    float m_rainIntensity = 0.0f;
    float m_snowIntensity = 0.0f;
    float m_temperatureC = 20.0f;
    Vec4 m_rainSpots[64] = {};
    uint32_t m_rainSpotCount = 0;
    bool m_rainSpotsLocal = false;
    // Cobertura da chuva: a grade emprestada do WeatherRenderer, ou a 1x1
    // zerada (nada cobre) enquanto ela nao existe.
    VkImage m_rainCoverFallbackImg = VK_NULL_HANDLE;
    VmaAllocation m_rainCoverFallbackAlloc = VK_NULL_HANDLE;
    VkImageView m_rainCoverFallbackView = VK_NULL_HANDLE;
    VkSampler m_rainCoverSampler = VK_NULL_HANDLE;
    VkImageView m_rainCoverBound = VK_NULL_HANDLE; // o que esta' escrito no binding 19
    VkImageView m_rainCoverView = VK_NULL_HANDLE;  // o pedido (nulo = a reserva)
    Vec4 m_rainCover = Vec4(0.0f);
    void bindRainCover();
    Vec4 m_wetAreas[kWetAreas] = {};
    uint32_t m_wetAreaCount = 0;
    VkImage m_shoreImg = VK_NULL_HANDLE;
    VmaAllocation m_shoreAlloc = VK_NULL_HANDLE;
    VkImageView m_shoreView = VK_NULL_HANDLE;
    VkSampler m_shoreSampler = VK_NULL_HANDLE;
    Vec4 m_shoreMask = Vec4(0.0f);
    // Copia da ultima grade da beira (setShoreMask): o resize() recria os
    // descritores e a imagem nasceria vazia (1x1) - a onda, a espuma e o
    // contato da lava sumiam ao maximizar a janela, ate' algo remontar a grade.
    std::vector<float> m_shorePx;
    uint32_t m_shoreW = 0, m_shoreH = 0;
    Vec2 m_shoreOrigin = Vec2(0.0f);
    float m_shoreInvSize = 0.0f;
    int m_shoreSwashMode = 0;
    Vec3 m_shoreWaterColor = Vec3(0.18f, 0.25f, 0.28f);
    float m_shoreClarity = 10.0f;
    float m_shoreWaterY = -1.0e9f;
    float m_shoreTime = 0.0f;
    Vec4 m_shoreLiquids[kShoreLiquids] = {};
    bool  m_ssaoEnabled = true;
    int   m_ssaoTaps = 12; // preset ssao_taps (especializacao do ssao.frag)
    int   m_ssaoShortTaps = 0;     // preset ssao_taps_short (especializacao 1 do ssao.frag)
    float m_ssaoShortRadius = 0.0f; // preset ssao_radius_short (u)
    bool  m_ssaoFullRes = false;    // preset ssao_full_res (alvos do SSAO do tamanho da tela)
    // True while the SSAO targets hold neutral white in SHADER_READ layout,
    // letting the per-frame chain be skipped entirely when SSAO is off.
    bool  m_ssaoNeutralCleared = false;
    float m_ssaoStrength = 0.8f;
    float m_ssaoRadius = 12.0f;  // unidades de MUNDO (celula de terreno = 10u)
    bool  m_envSpecEnabled = true;
    float m_envSpecIntensity = 1.0f;
    float m_nightAoPow = 1.6f;
    float m_ambientHemiFloor = 0.35f;
    float m_fillLightIntensity = 0.25f;
    float m_microShadow = 1.0f;
    float m_microShadowContrast = 1.5f;
    float m_microShadowFloor = 0.35f;
    bool  m_sunBounceEnabled = true;
    float m_sunBounceStrength = 0.6f;
    bool  m_contactShadowEnabled = true;
    float m_contactShadowLength = 1.2f;
    Mat4  m_cameraViewProj = Mat4(1.0f);

    // Dedicated half-res SSAO chain: raw pass -> bilateral blur -> sampled by
    // the lighting passes at binding 13 (raw at 12 for the blur).
    VkImage m_ssaoImage = VK_NULL_HANDLE;
    VmaAllocation m_ssaoAlloc = VK_NULL_HANDLE;
    VkImageView m_ssaoView = VK_NULL_HANDLE;
    VkImage m_ssaoBlurImage = VK_NULL_HANDLE;
    VmaAllocation m_ssaoBlurAlloc = VK_NULL_HANDLE;
    VkImageView m_ssaoBlurView = VK_NULL_HANDLE;
    VkPipeline m_ssaoPipeline = VK_NULL_HANDLE;
    VkPipeline m_ssaoBlurPipeline = VK_NULL_HANDLE;
    uint32_t m_ssaoWidth = 0, m_ssaoHeight = 0;

    void createLitImage();
    void createLightVolumeGeometry();
    void createPipelines();
    void createSsaoPipelines();
    void createDescriptors();
    void updateDescriptorSet(VkImageView shadowView, VkSampler shadowSampler,
                             VkImageView nextShadowView, VkSampler nextShadowSampler,
                             VkImageView cloudShadowView, VkSampler cloudShadowSampler,
                             VkImageView rainOcclusionView, VkSampler rainOcclusionSampler,
                             uint32_t frameIndex);

    void renderAmbientPass(VkCommandBuffer cmd);
    void renderDirectionalPass(VkCommandBuffer cmd, const Camera& camera, const ShadowRenderer* shadows,
                               VkImageView cloudShadowView, VkSampler cloudShadowSampler,
                               const Vec3& cloudWorldMin, const Vec3& cloudWorldMax,
                               float cloudShadowIntensity,
                               VkImageView rainOcclusionView, VkSampler rainOcclusionSampler,
                               float totalTime);
    void renderPointLights(VkCommandBuffer cmd, const Camera& camera);
    void pickPointLights(const Camera& camera);
    static void fillLightTiles(LightingUBO& ubo, const Vec3& focus, uint32_t count, uint32_t perTile);
};

} // namespace eruption
