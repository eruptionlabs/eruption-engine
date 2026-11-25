#pragma once
#include "renderer/PushRing.hpp"

#include "renderer/VulkanContext.hpp"
#include "renderer/GBuffer.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "math/Types.hpp"

#include <vector>
#include <cstdint>

namespace eruption {

class ShadowRenderer;

struct SpriteInstanceData {
    Mat4 modelMatrix;
    Vec4 anchorPoint; // xyz = world anchor, w = padding/unused
    Vec4 texRect;
    uint32_t texIndex;
    uint32_t normalTexIndex;
    uint32_t mrahwTexIndex;
    uint32_t flags; // bit 0: flipX, bit 1: flipY, bit 2: billboard, bit 3-7: matId, bit 8: usePalette
    uint32_t paletteIndex;
    Vec4 tintColor;
};

struct FrameUBO {
    Mat4 view;
    Mat4 projection;
    Mat4 viewProjection;
    Mat4 inverseView;
    Mat4 inverseProjection;
    Vec3 cameraPos;
    float time;
    Vec2 screenResolution;
    float nearPlane;
    float farPlane;
    uint32_t frameIndex;
    uint32_t debugMode;
    float spriteExposure; 
    float giIntensity;     // Added to match map lighting
    float giAmbientFloor;  // Added to match map lighting
    float spriteTilt;      // isometric tilt in degrees
    float shadowHeightScale; // Compress shadow height
    float spriteNormalYMix;  // Mix for Y-up normal
    float normalMapScale;    // Strength of the normal map detail (0 = flat, 1 = full)
    float normalMapInvertY;  // 0 = OpenGL/Vulkan convention (Y up), 1 = flip green channel (Y down)
    float normalSmoothing;   // LOD bias for normal maps to smooth compression artifacts
    float defaultRoughness;  // Fallback roughness when no PBR map is bound
    float defaultMetallic;   // Fallback metallic when no PBR map is bound

    // Lighting for Forward Sprites
    alignas(16) Vec4 sunDir;          // xyz=dir, w=intensity
    alignas(16) Vec4 sunColor;        // xyz=color, w=unused
    alignas(16) Vec4 ambientSky;      // xyz=skyColor, w=ambientIntensity
    alignas(16) Vec4 ambientGround;   // xyz=groundColor, w=unused

    // Appended fields: only the shaders that use them declare this tail
    // (model.frag/terrain.frag); shorter shader-side blocks stay valid.
    // POM (banda média do displacement híbrido - docs/displacement_design.md):
    // x=heightScale (uv units), y=maxSteps, z=minSteps, w=wetness global
    // (tira o dual-use do alpha do MRAH-W, que agora é SÓ altura).
    alignas(16) Vec4 pomParams;

    // VENTO (IGNIS). xyz = vetor de vento em mundo (direcao * forca, ja' com a
    // rajada), w = tempo do vento em segundos - proprio, e nao `time`, porque
    // o vento precisa de um relogio que nao reinicie com o mapa.
    //
    // Anexado no fim, como os anteriores: os offsets de tudo acima ficam
    // intactos e um shader que declare um bloco mais curto continua valido.
    alignas(16) Vec4 windParams;

    // RELEVO POR DISTANCIA (ideia do autor, 2026-09-06). Duas curvas editadas
    // no F2 com o widget de spline da engine (SplineEditorUI, o mesmo do DoF e
    // das sombras) e enviadas como LUT de 8 AMOSTRAS uniformes em
    // [0, normalDistParams.x] unidades de mundo. LUT em vez dos pontos da
    // curva: assim o autor pode por quantos pontos quiser sem mexer no UBO nem
    // no shader, e o shader faz uma interpolacao linear de 8 valores.
    //   normalDistParams: x = distancia maxima (u), y = 1 liga / 0 desliga
    //   normalScaleLut0/1  = forca do normal map      (8 amostras)
    //   normalSmoothLut0/1 = vies de mip do normal map (8 amostras)
    alignas(16) Vec4 normalDistParams;
    alignas(16) Vec4 normalScaleLut0;
    alignas(16) Vec4 normalScaleLut1;
    alignas(16) Vec4 normalSmoothLut0;
    alignas(16) Vec4 normalSmoothLut1;

    // TESSELACAO DE HARDWARE NA BANDA PERTO (model.tesc/model.tese).
    //   x = distancia maxima da curva (u)
    //   y = 1 liga / 0 desliga
    //   z = amplitude do deslocamento em unidades de mundo
    //   w = fator maximo da curva (normaliza a atenuacao da amplitude)
    alignas(16) Vec4 tessParams;
    alignas(16) Vec4 tessLut0;
    alignas(16) Vec4 tessLut1;
    // x = reservado.
    // y = espacamento alvo entre vertices da tesselacao, em u (0 = so' a
    //     curva, como antes) - ver densityFactor() em model.tesc.
    // z, w = slope cap (model.frag) / foliage cap (model.tesc).
    alignas(16) Vec4 tessParams2;
    // Reservado (era o vies de mip fixo da altura, "Height Blur", que saiu).
    // Mantem o layout.
    float framePad0;
    // GRADE POR DISTANCIA da tesselacao (model.tesc/model.vert/model.tese):
    // ate' tessNearDistance u a grade usa tessNearSpacing e o piso de
    // tessNearEdgePixels px de aresta; dali em diante a de antes
    // (tessParams2.y e 8 px). 0 = sem banda perto. Cabem no preenchimento
    // antes da Mat4 alinhada abaixo: nenhum offset muda.
    float tessNearEdgePixels;
    float tessNearDistance;
    float tessNearSpacing;

    // MATRIZES SEM JITTER (FSR). As matrizes do inicio do bloco (projection,
    // viewProjection, inversas) sao as de RASTERIZACAO: com jitter quando ele
    // esta ligado, e o depth do G-buffer sai delas. Estas sao as de logica e
    // de motion vectors. Anexadas no fim como as demais.
    alignas(16) Mat4 viewProjNoJitter;
    alignas(16) Mat4 prevViewProjNoJitter;
    // xy = jitter deste frame, zw = do anterior, em NDC do Vulkan (y para baixo).
    alignas(16) Vec4 jitterNdc;
    // windParams do frame ANTERIOR: a vegetacao reconstroi onde estava (velocidade de objeto).
    alignas(16) Vec4 prevWindParams;
    // VER ATRAVES (camera.see_through): o que fica entre a camera e o jogador,
    // dentro de uma elipse em volta dele na tela, sai em dither no G-buffer
    // das malhas (gbuffer/see_through.glsl). seeThrough: xy = centro do
    // jogador em NDC, zw = raios da elipse em NDC (z <= 0 desliga).
    // seeThrough2: x = distancia camera-jogador, y = altura dos pes,
    // z = altura do sprite, w = opacidade minima no centro.
    alignas(16) Vec4 seeThrough;
    alignas(16) Vec4 seeThrough2;
    // xy = pixel (do alvo do G-buffer) de um ponto FIXO do mundo perto do
    // jogador: o padrao do dither anda com o mundo, nao com a tela.
    alignas(16) Vec4 seeThrough3;
    // JUNCAO MALHA x TERRENO (Engine::fillGroundBlend, gbuffer/ground_blend.glsl).
    // gbMaskU/V: UV da mascara do splat = (a*x + b*z + c); .w = faixa (0 desliga)
    // e o lado da celula da grade de altura. gbUvU/V: UV do chao, idem; .w =
    // altura minima e faixa de altura da textura. gbHeight: UV da textura de
    // altura = (x * .x + .y, z * .z + .w). gbSplat: slots das 8 camadas (2 por
    // uint, com as flags). gbSlots: mascara, mascara 2, textura de altura, cor
    // base do chao. gbSlots2: normal e MRAH-W base do chao.
    alignas(16) Vec4 gbMaskU;
    alignas(16) Vec4 gbMaskV;
    alignas(16) Vec4 gbUvU;
    alignas(16) Vec4 gbUvV;
    alignas(16) Vec4 gbHeight;
    alignas(16) uint32_t gbSplat[4];
    alignas(16) uint32_t gbSlots[4];
    alignas(16) uint32_t gbSlots2[4];
    // RECORTE POR VOLUME (world.set_cutaway, gbuffer/see_through.glsl): ate'
    // kCutaways volumes, 3 vec4 cada. a = centro + forma (w: 0 desligado,
    // 1 caixa, 2 cilindro); b = meia largura/raio, meia altura, meia
    // profundidade, borda suave; c = cos e sen do giro em Y, opacidade.
    static constexpr uint32_t kCutaways = 4;
    alignas(16) Vec4 cutaway[3 * kCutaways];
    // Faixa do ver atraves abaixo do foco (gbuffer/see_through.glsl): x =
    // distancia de corte (0 desliga), y = sinal do "para baixo" em NDC, z =
    // meia largura em NDC.
    alignas(16) Vec4 seeThroughBand;
    // Ver atraves com o centro no foco: uma elipse por personagem perto dele
    // (Render.cpp), 2 vec4 cada: o primeiro como seeThrough (z <= 0 = fim
    // da lista); o segundo como seeThrough2, mas x = distancia no plano XZ
    // da camera aos pes.
    static constexpr uint32_t kSeeThroughExtra = 6;
    alignas(16) Vec4 seeThroughExtra[2 * kSeeThroughExtra];
    // x = forca do alfa-teste por hash (chave de preset alpha_hash), 0 = limiar fixo de sempre.
    alignas(16) Vec4 alphaHash;
    // x = espacamento minimo (u) do filtro da altura da tesselacao (chave
    // tessellation_height_min_spacing; 0 = so' a grade). model.vert/model.tese.
    // y = relief_cavity, z = relief_cavity_contrast, w = normal_strength
    // (ilusao barata de relevo, model.frag).
    alignas(16) Vec4 reliefParams;
    // SPRITES (gbuffer/sprite.frag, sprite_layer.frag): x = filtro (chave de
    // preset sprite_filter: 0 nearest, 1 bilinear nitido), y = forca do normal
    // map AUTORAL do sprite (sprite_normal_strength; 0 = sem). zw reservados.
    alignas(16) Vec4 spriteParams;
    // LUZ DE BORDA do lado da luz (gbuffer/sprite.frag + lighting/directional.frag):
    // rgb = tinta, w = forca efetiva (characters.set_rim x preset sprite_rim; 0 desliga).
    alignas(16) Vec4 spriteRim;
    // DEBUG DE SPLINE NO MAPA (SplineCurve::debug; gbuffer/model_frag.glsl). Uma
    // spline de mundo com "Debug Mode" ligado por vez (a primeira da ordem do
    // Render.cpp). [0] = x liga (0 = desligado: nenhum tingimento), y = numero
    // de pontos (<= 8), z = maior valor da curva, w = eixo x maximo em unidades
    // de mundo. [1], [2] = x normalizado de cada ponto. [3], [4] = a curva em 8
    // amostras (como as LUTs acima). [5..12] = cor de cada ponto (rgb).
    alignas(16) Vec4 splineDbg[13];
    // POM DO CHAO POR PRESET (gbuffer/model_frag.glsl, reliefPomPreset; chaves
    // "relief_pom*"; independe da tesselacao ao vivo, entao vale com
    // relief_bake "mesh"). reliefPom: x = espessura da camada (u; 0 desliga),
    // y = passos maximos, z = passos minimos, w = distancia onde o POM some (u).
    alignas(16) Vec4 reliefPom;
    // x = contraste da altura em volta da media local, y = forca do
    // escurecimento da fresta pela profundidade do POM (0 desliga), z = fator do
    // filtro da altura pela grade real da tesselacao (model.tese).
    alignas(16) Vec4 reliefPom2;
    // Relevo sem tesselacao (model_frag.glsl; 0 = desliga). reliefPom3: x =
    // espessura da altura que vira normal, y = forca da auto-sombra ao sol,
    // z = passos da marcha, w = distancia onde some. reliefPom4: x = reforco
    // do POM perto da camera, y = distancia desse reforco, z = espessura da
    // camada da auto-sombra, w = alcance maximo da sombra (u).
    alignas(16) Vec4 reliefPom3;
    alignas(16) Vec4 reliefPom4;
    // Calibracao de albedo na escrita do G-buffer (gbuffer/albedo_calib.glsl):
    // x = albedo_knee (luminancia linear onde a compressao comeca; <= 0
    // desliga), y = albedo_ceiling (teto da luminancia). Anexado no fim: nenhum
    // offset antigo muda e UBO zerado = desligado.
    alignas(16) Vec4 albedoCalib;
    // Brilho dos objetos perto de luzes (model_frag.glsl): glowInfo.x = quantas
    // das 8 luzes valem (0 = desligado, saida identica); glowPos = xyz posicao,
    // w raio; glowColor = rgb cor, w forca (ja com cintilacao e horario).
    alignas(16) Vec4 glowInfo;
    alignas(16) Vec4 glowPos[8];
    alignas(16) Vec4 glowColor[8];
    // Cintilacao das texturas emissivas das regioes: ate' 64 pares (slot da
    // textura emissiva, fator); emiFlickerInfo.x = quantos pares valem.
    alignas(16) Vec4 emiFlickerInfo;
    alignas(16) Vec4 emiFlicker[32]; // xy = par 2k, zw = par 2k+1
    // Rajadas locais de vento (world.wind_burst / afeccoes, core/Affects.hpp):
    // gustInfo.x = quantas das 8 valem (0 = o shader sai antes, imagem identica);
    // gusts[2k] = (x, z, raio, forca de agora), gusts[2k+1] = (frente 0..1 do
    // raio, largura da frente, 0, 0). Lidas so' pelo vertex shader de vegetacao.
    alignas(16) Vec4 gustInfo;
    alignas(16) Vec4 gusts[16];
    // Tempo de cada rajada, para a mola da copa: (duracao s, idade s, 0, 0).
    alignas(16) Vec4 gustsT[8];
};
// 16 KiB e' o minimo que a Vulkan garante para um UBO (maxUniformBufferRange).
static_assert(sizeof(FrameUBO) <= 16384, "FrameUBO passou do UBO minimo garantido");

// Luz pontual dominante de um sprite (Engine::spriteDominantLights), por
// instancia do buffer de sprites. 32 B, o mesmo layout std430 do lado da GPU.
struct SpriteLight {
    Vec3 position = Vec3(0.0f);
    float strength = 0.0f; // 0 = nenhuma luz pontual perto
    Vec3 color = Vec3(0.0f);
    float pad = 0.0f;
};
static_assert(sizeof(SpriteLight) == 32, "SpriteLight: layout std430 de 32 B");

// Sombra de um sprite a partir de uma luz pontual (renderBillboardPointShadows):
// instancia no buffer de sprites, posicao da luz e opacidade.
struct SpritePointShadow {
    uint32_t instance = 0;
    Vec3 light = Vec3(0.0f);
    float strength = 0.0f;
};

// Decal no chao (characters.ring e vfx.area): push constant do
// ground_ring.vert/.frag, um desenho por forma. Forma 0 e' a coroa (setor
// de coroa: circulo, anel, cone, arco), forma 1 o retangulo (linha).
struct GroundRing {
    Vec4 center = Vec4(0.0f); // xyz = pe', w = raio externo (linha: comprimento)
    Vec4 color = Vec4(1.0f);  // rgb, a = alfa
    Vec4 shape = Vec4(0.0f);  // x = borda, y = folga acima, z = folga abaixo, w = forma (+2: no plano de fill.zw)
    Vec4 dir = Vec4(0.0f, 1.0f, 4.0f, 0.0f); // xy = direcao (x, z), z = meia abertura (rad), w = raio interno / meia largura
    Vec4 fill = Vec4(1.0f, 1.0f, 0.0f, 0.0f); // x = progresso 0..1, y = alfa do miolo, zw = inclinacao (x, z) do plano
};

class SpriteRenderer {
public:
    bool init(VulkanContext* ctx, GBuffer* gbuffer, BindlessDescriptor* bindless);
    void shutdown();

    void addSprite(const SpriteInstanceData& sprite);
    void clearSprites();
    void flushUploads(VkCommandBuffer cmd);

    // Palette management
    uint32_t addPalette(const uint8_t* rgba256);
    void updatePalette(uint32_t slot, const uint8_t* rgba256);
    void removePalette(uint32_t slot);

    // Update the shared FrameUBO buffer without drawing. Call this before
    // terrain/model geometry passes so they read current camera/POM data.
    void updateFrameUbo(const FrameUBO& frameUbo);

    void renderSprites(VkCommandBuffer cmd, const FrameUBO& frameUbo);
    void renderSprites(VkCommandBuffer cmd, const FrameUBO& frameUbo, VkBuffer instanceBuffer, uint32_t count);

    // Sprites no G-buffer do passe espelhado da agua (rendering ja' aberto
    // pelo GBuffer do passe): cada billboard vira um cartao em pe' na ancora,
    // de frente (na horizontal) para a camera. mirrorViewProj leva o MUNDO ao
    // recorte espelhado; billboardRight/towardCamera sao horizontais e unitarios.
    void renderMirror(VkCommandBuffer cmd, const Mat4& mirrorViewProj, const Vec3& billboardRight,
                      const Vec3& towardCamera, float normalUpMix, VkBuffer instanceBuffer, uint32_t count,
                      float minAnchorY = -1.0e30f); // ancora abaixo disto sobe ate' aqui (reflexo de altura inteira)

    void renderSpritesForward(VkCommandBuffer cmd, const FrameUBO& frameUbo, VkBuffer instanceBuffer, uint32_t count, const ShadowRenderer* shadows);

    void renderShadow(VkCommandBuffer cmd, VkPipelineLayout shadowLayout, const Mat4& lightSpaceMatrix, const FrameUBO& frameUbo, VkBuffer instanceBuffer, uint32_t count);
    
    // Projected Planar Shadow for billboards — renders a flattened sprite silhouette on the ground.
    void renderBillboardPlanarShadows(VkCommandBuffer cmd, const FrameUBO& frameUbo, const Vec3& shadowLightDir,
                                      const Vec2& planarParams, VkBuffer instanceBuffer, uint32_t count);

    // Sombra de disco no pe' como DECAL: avalia a pegada no ponto visivel lido
    // de `sceneDepth` (shader read-only). Fora do passe forward, num trecho so'
    // com a imagem lit (sem anexo de profundidade).
    // `pointShadows` (sombra por luz pontual, characters.set_shadow("lights"))
    // sai no mesmo trecho, tambem como decal: billboard_point_shadow.vert.
    // Amplitude do relevo no chao visivel (u): a folga de altura do disco
    // soma ela, senao o chao deslocado sai da janela e o disco some.
    void setDecalReliefSlack(float slack) { m_decalReliefSlack = slack; }
    // pbr do G-buffer: o disco pula o pixel de sprite (r = 0,75), que grava
    // a profundidade do chao e cairia na janela de altura alargada.
    void setDecalGbufferPbr(VkImageView pbr) { m_decalGbufferPbr = pbr; }
    void renderBillboardCircleShadows(VkCommandBuffer cmd, const FrameUBO& frameUbo,
                                      const Vec2& circleParams, VkBuffer instanceBuffer, uint32_t count,
                                      VkImageView sceneDepth, const Vec2& planarParams,
                                      const std::vector<SpritePointShadow>& pointShadows,
                                      VkImageView sunShadowDepth, VkBuffer sunShadowUbo, const Vec4& sunGate);

    // Aneis de chao como decal (ground_ring.vert): trecho so' com a imagem
    // lit, depois dos discos; `sceneDepth` e `gbufferPbr` (marca de sprite)
    // em leitura.
    void renderGroundRings(VkCommandBuffer cmd, const FrameUBO& frameUbo, const std::vector<GroundRing>& rings,
                           VkImageView sceneDepth, VkImageView gbufferPbr);

    uint32_t spriteCount() const { return m_spriteCount; }

    void setSpriteExposure(float exposure) { m_spriteExposure = exposure; }
    float spriteExposure() const { return m_spriteExposure; }

    // Expose the shared FrameUBO descriptor so other G-Buffer pipelines
    // (model, terrain) can bind the same set 1 and access camera/time/POM data.
    VkDescriptorSetLayout frameUboLayout() const { return m_frameUboLayout; }
    // Luz principal de cada sprite (Engine::spriteDominantLights, indice =
    // instancia): o sprite.frag le' para a contraluz da borda (binding 1 do
    // set do FrameUBO). Sprites alem da capacidade ficam sem luz.
    void uploadSpriteLights(const std::vector<SpriteLight>& lights);
    VkDescriptorSet frameUboSet() const { return m_frameUboSet; }

    // CAMADA DE SPRITES do FSR (ver SpriteLayer). createLayerPipelines recebe
    // o layout do set 2 da camada (depth, iluminado e albedo de render).
    struct LayerPush {
        Mat4 view;
        Mat4 proj;      // SEM jitter
        Vec4 sizes;     // xy = render, zw = display
        Vec4 planes;    // x = near, y = far
    };
    bool createLayerPipelines(VkDescriptorSetLayout layerInputs);
    // Cobertura (R8) na resolucao de render; rendering ja' aberto pelo
    // chamador com o alvo da mascara + depth do G-buffer somente leitura.
    void drawMask(VkCommandBuffer cmd, VkBuffer instances, uint32_t count, const Mat4& viewProjJittered);
    // Sprites na resolucao de display; rendering ja' aberto com o alvo RGBA16F.
    void drawLayer(VkCommandBuffer cmd, VkBuffer instances, uint32_t count, VkDescriptorSet layerInputs,
                   const LayerPush& push, VkExtent2D extent);

private:
    VulkanContext* m_ctx = nullptr;

    struct PendingPaletteUpload {
        VkBuffer stagingBuffer;
        VmaAllocation stagingAlloc;
        VkImage targetImage;
    };
    std::vector<PendingPaletteUpload> m_pendingPaletteUploads;
    
    // G-Buffer
    GBuffer* m_gbuffer = nullptr;
    BindlessDescriptor* m_bindless = nullptr;

    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipeline m_forwardPipeline = VK_NULL_HANDLE;
    VkPipeline m_mirrorPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_mirrorLayout = VK_NULL_HANDLE;
    struct MirrorPush {
        Mat4 viewProjection;
        Vec4 right;  // xyz = direita do billboard, w = mistura da normal com +Y
        Vec4 facing; // xyz = horizontal ate' a camera, w = altura minima da ancora
    };
    static_assert(sizeof(MirrorPush) <= 128, "push do sprite espelhado passa de 128 bytes");
    VkPipeline m_maskPipeline = VK_NULL_HANDLE;
    VkPipeline m_layerPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layerPipelineLayout = VK_NULL_HANDLE;
    PushRing m_layerPushRing; // LayerPush (160 B) no set 3: passa dos 128 de push constant
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
    VkPipeline m_planarShadowPipeline = VK_NULL_HANDLE;
    VkPipeline m_circleShadowPipeline = VK_NULL_HANDLE;
    VkPipeline m_pointShadowPipeline = VK_NULL_HANDLE; // decal da sombra de luz pontual
    // Decal do disco: sets 0-1 iguais aos do sprite + set 2 com a profundidade,
    // um por quadro em voo (escrito antes do desenho, a imagem muda com a escala).
    static constexpr uint32_t kDecalFrames = 3;
    VkDescriptorSetLayout m_decalDepthLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_decalDepthPool = VK_NULL_HANDLE;
    VkDescriptorSet m_decalDepthSets[kDecalFrames] = {};
    VkSampler m_decalDepthSampler = VK_NULL_HANDLE;
    VkPipelineLayout m_decalPipelineLayout = VK_NULL_HANDLE;
    float m_decalReliefSlack = 0.0f;
    VkImageView m_decalGbufferPbr = VK_NULL_HANDLE;
    // Anel: set 2 proprio (profundidade e pbr do G-buffer). O do disco ja' foi ligado no
    // quadro; reescreve-lo depois invalidaria o command buffer.
    VkPipeline m_ringPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_ringPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ringDepthLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_ringDepthPool = VK_NULL_HANDLE;
    VkDescriptorSet m_ringDepthSets[kDecalFrames] = {};
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

    VkBuffer m_quadVertexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_quadVertexAlloc = VK_NULL_HANDLE;

    VkBuffer m_instanceBuffer = VK_NULL_HANDLE;
    VmaAllocation m_instanceAlloc = VK_NULL_HANDLE;
    void* m_mappedInstances = nullptr;

    VkBuffer m_frameUboBuffer = VK_NULL_HANDLE;
    VmaAllocation m_frameUboAlloc = VK_NULL_HANDLE;
    void* m_mappedFrameUbo = nullptr;
    VkBuffer m_spriteLightBuffer = VK_NULL_HANDLE; // SpriteLight por instancia (m_maxInstances)
    VmaAllocation m_spriteLightAlloc = VK_NULL_HANDLE;
    void* m_mappedSpriteLights = nullptr;
    size_t m_spriteLightsUsed = 0;

    VkDescriptorSetLayout m_frameUboLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_frameUboSet = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_shadowLayoutSet = VK_NULL_HANDLE;
    VkDescriptorSet m_shadowSet = VK_NULL_HANDLE;

    VkSampler m_paletteSampler = VK_NULL_HANDLE;

    struct PaletteTexture {
        VkImage image;
        VmaAllocation alloc;
        VkImageView view;
        uint32_t bindlessSlot;
    };
    std::vector<PaletteTexture> m_palettes;

    float m_spriteExposure = 1.0f;
    uint32_t m_maxInstances = 10000;
    uint32_t m_spriteCount = 0;

    bool createQuadBuffer();
    bool createBuffers();
    bool createDescriptors();
    bool createPipeline();
    bool createForwardPipeline();
    bool createMirrorPipeline();
    bool createShadowPipeline();
    bool createPlanarShadowPipeline();
    bool createCircleShadowPipeline();
    VkPipeline createDecalPipeline(const std::string& shaderBase);
    bool createRingPipeline();
};

} // namespace eruption
