#pragma once

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

struct PointLight {
    Vec3 position = Vec3(0.0f);
    float radius = 10.0f;
    Vec3 color = Vec3(1.0f);
    float intensity = 1.0f;
    LightAnimType animType = LightAnimType::Static;
    uint32_t shadowCubemapIndex = 0xFFFFFFFF;
    bool enabled = true;
    // When true, Engine toggles `enabled` off/on by time of day instead of
    // leaving it always on (lamp/lantern props: lit only at night).
    bool nightOnly = false;
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
    uint32_t pad1;
    alignas(16) Vec4 ambientSky;        // xyz=skyColor, w=unused
    alignas(16) Vec4 ambientGround;     // xyz=groundColor, w=unused
    alignas(16) Vec4 ssaoParams;        // x=enabled(0/1), y=strength, z=radius, w=unused
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
    void setGlobalIllumination(float intensity, float floor) { m_giIntensity = intensity; m_giAmbientFloor = floor; }
    void setSunAngularRadius(float radians) { m_sunAngularRadius = radians; }
    void setPointLights(const std::vector<PointLight>& lights);
    // Gate do passe inteiro. Antes NADA lia o RenderEffect "Point Lights" nem
    // a chave "point_lights" do preset (o `low` dizia false e as luzes
    // desenhavam do mesmo jeito) - o mesmo defeito da caixa de bloom.
    void setPointLightsEnabled(bool on) { m_pointLightsEnabled = on; }
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

    void setUsePbr(bool use) { m_usePbr = use; }
    bool usePbr() const { return m_usePbr; }
    void setPbrDebugMode(uint32_t mode) { m_pbrDebugMode = mode; }
    uint32_t pbrDebugMode() const { return m_pbrDebugMode; }
    void setPbrLightScale(float scale) { m_pbrLightScale = scale; }
    float pbrLightScale() const { return m_pbrLightScale; }

    // Indirect-lighting controls (inline contact SSAO + analytic env specular +
    // ambient shaping). Driven by the Lighting tab.
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
    void setFillLight(float intensity) { m_fillLightIntensity = intensity; }
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
    bool m_pointLightsEnabled = true;
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
    float m_giIntensity = 1.0f;
    float m_giAmbientFloor = 0.10f;
    bool m_usePbr = [] { return std::getenv("ERUPTION_TEST_NO_PBR") == nullptr; }();
    Vec4 m_shadowOpacityParams{1000.0f, 0.0f, 0.0f, 0.0f};
    Vec4 m_shadowOpacityLut0{1.0f, 1.0f, 1.0f, 1.0f};
    Vec4 m_shadowOpacityLut1{1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t m_pbrDebugMode = 0;
    float m_pbrLightScale = 2.0f;
    float m_rainIntensity = 0.0f;
    float m_snowIntensity = 0.0f;
    float m_temperatureC = 20.0f;
    bool  m_ssaoEnabled = true;
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
};

} // namespace eruption
