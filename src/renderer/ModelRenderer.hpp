#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "renderer/MipmapGenerator.hpp"
#include "renderer/ModelData.hpp"
#include "utils/PbrTextureLoader.hpp"
#include "utils/PbrMaterialProfile.hpp"
#include "math/Frustum.hpp"
#include "math/CullingSIMD.hpp"
#include "utils/AlignedAllocator.hpp"

#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <cstdint>

namespace eruption {

struct ModelMeshGPU {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    // PARTICAO ESPACIAL DO CHAO (para a tesselacao da banda perto). O chao e'
    // UMA malha de centenas de milhares de triangulos: mandar a malha inteira
    // pelo pipeline de patch custa 304 ms (medido) porque CADA patch paga o
    // estagio, mesmo com fator 1. Com os indices reordenados por celula, so' as
    // celulas que caem na banda vao pelo caminho de tesselacao (~1 ms) e o
    // resto do chao continua no desenho normal.
    struct GroundCell { float minX, minZ, maxX, maxZ; uint32_t first, count; };
    std::vector<GroundCell> groundCells;
    uint32_t bindlessTexSlot = 0; // primary texture
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;

    // PBR material scales resolved at load time to avoid CPU bottleneck during rendering.
    float metallicScale = 0.0f;
    // Fator de deslocamento da tesselacao, por material (ver createPipeline /
    // o bloco de categoria no load). 0 = superficie dura, nunca desloca.
    float dispScale = 0.35f;
    // UV por unidade de mundo (mediana dos triangulos). Vai no push.uvScale.z
    // pro shader escolher o mip da altura pela grade de vertices (Nyquist) -
    // ver dispMipFloor() em model.vert/model.tese.
    float uvPerUnit = 0.0f;
    float roughnessScale = 1.0f;
    // Quanto esta malha balanca ao vento, 0 = rigida. Resolvido no LOAD a
    // partir da categoria do material, pelo mesmo caminho de metallic/roughness
    // - folhagem balanca, pedra e parede nao.
    float swayAmount = 0.0f;

    Vec3 aabbMin;
    Vec3 aabbMax;
    bool isExternal = false; // lifecycle managed elsewhere

    // LOD geométrico estilo mipmap: malha subdividida + passa-baixa usada
    // quando a câmera está perto. Gerada no LOAD; trocar de nível em runtime
    // é só desenhar outro buffer, então zoom in/out não gera hitch.
    VkBuffer hiVertexBuffer = VK_NULL_HANDLE;
    VmaAllocation hiVertexAlloc = VK_NULL_HANDLE;
    VkBuffer hiIndexBuffer = VK_NULL_HANDLE;
    VmaAllocation hiIndexAlloc = VK_NULL_HANDLE;
    uint32_t hiIndexCount = 0;
    float hiSwitchDistance = 0.0f; // 0 = sem LOD fino

    // LOD GROSSEIRO por distância (meshoptimizer). Só um index buffer novo
    // sobre o MESMO vertex buffer - a instância longe desenha menos triângulo
    // sem custar uma segunda cópia da malha. Motivo medido: o parana_field
    // submete até 3,9M de triângulos por frame vindos de 713 instâncias.
    VkBuffer loIndexBuffer = VK_NULL_HANDLE;
    VmaAllocation loIndexAlloc = VK_NULL_HANDLE;
    uint32_t loIndexCount = 0;
    float loSwitchDistance = 0.0f; // 0 = sem LOD grosseiro

    // LOD DISTANTE (nivel 3): terceiro index buffer, bem mais agressivo que o
    // "lo". Motivo medido no parana_field com zoom 0 (dist 1000): 14,7 M de
    // triangulos por frame em 0,9 Mpx - arvores de 106 k tris caiam no "lo"
    // de 12 k e ainda assim 227 instancias delas somavam 2,7 M. A 1000
    // unidades uma arvore tem ~20 px: 12 k triangulos SUB-PIXEL nao viram
    // detalhe, viram RUIDO que muda a cada frame (o "shimmering" do zoom 0 -
    // o A/B com sombra desligada provou que 92% do ruido nao era sombra).
    VkBuffer farIndexBuffer = VK_NULL_HANDLE;
    VmaAllocation farIndexAlloc = VK_NULL_HANDLE;
    uint32_t farIndexCount = 0;
    float farSwitchDistance = 0.0f; // 0 = sem LOD distante

    // VERTICES PROPRIOS dos niveis 2/3 quando o LOD e' poda de cartoes com
    // compensacao de area (pruneCardsAreaPreserving): os cartoes que ficam sao
    // ESCALADOS, entao o index buffer sozinho nao descreve o nivel.
    // VK_NULL_HANDLE = o nivel usa vertexBuffer (caminho classico).
    VkBuffer loVertexBuffer = VK_NULL_HANDLE;
    VmaAllocation loVertexAlloc = VK_NULL_HANDLE;
    VkBuffer farVertexBuffer = VK_NULL_HANDLE;
    VmaAllocation farVertexAlloc = VK_NULL_HANDLE;

    // LOD EXCLUSIVO DA SOMBRA, bem mais agressivo que o da tela. Medido: trocar
    // a malha da sombra pelo LOD dá RMS 0,000 - a silhueta simplificada projeta
    // nos MESMOS texels do shadow map, que é bem mais grosseiro que a tela.
    // Então a sombra pode pagar um LOD que seria inaceitável no passe visível.
    VkBuffer shIndexBuffer = VK_NULL_HANDLE;
    VmaAllocation shIndexAlloc = VK_NULL_HANDLE;
    uint32_t shIndexCount = 0;

    // CHAO (G38): malha de terreno vinda como modelo (mapa GLB: material de chao,
    // categoria "ground" ou vertices com splat). O bake das probes de
    // irradiancia renderiza o chao SEPARADO dos outros modelos - visto de
    // baixo, o chao seria a "base" de toda laje e a arvore inteira, do chao a'
    // copa, viraria solida.
    bool isGround = false;
    // Caster SOLIDO: nenhuma textura desta malha tem alfa, entao a sombra dela
    // pode ser desenhada por uma pipeline SEM fragment shader (o teste de alfa
    // nunca descartaria nada). Ver BindlessDescriptor::slotOpaque e
    // ShadowRenderer::shadowInstOpaquePipeline.
    bool shadowOpaque = false;

    void shutdown(VulkanContext* ctx);
    // Libera TODOS os buffers da malha (vertex, index, hi/lo/far x2, sh),
    // sem olhar isExternal, e devolve os bytes liberados. E' a UNICA lista de
    // buffers que existe: shutdown() chama isto depois da guarda de
    // isExternal, e o cache de assets do Engine (que e' o dono das malhas
    // externas) chama isto direto. Antes o cache tinha duas copias a mao da
    // lista - com 4 dos 9 buffers - e os de LOD/sombra vazavam no shutdown:
    // 48 index buffers vivos, VMA abortando em build de debug.
    VkDeviceSize destroyBuffers(VulkanContext* ctx);
    // Os dois grupos de propriedade - ver o comentario em shutdown().
    VkDeviceSize destroyBaseBuffers(VulkanContext* ctx); // vertex/index/hi*: do cache quando isExternal
    VkDeviceSize destroyLodBuffers(VulkanContext* ctx);  // lo*/far*/sh*: sempre do ModelRenderer
};

struct ModelInstance {
    uint32_t meshIndex = 0; // index into m_meshes
    Mat4 transform = Mat4(1.0f);
    bool enabled = true;
    std::string name;
    float boundingRadius = 0.0f; // world-space bounding radius for shadow LOD
    
    // Optimized culling data
    Vec3 worldAabbMin = Vec3(0.0f);
    Vec3 worldAabbMax = Vec3(0.0f);
    Vec3 worldCenter = Vec3(0.0f);

    // Metadata for remapping/caching
    std::string assetPath;
    uint32_t nodeIndex = 0;

    // Animated texture (UV) state. Updated per-frame for nodes with animTextures.
    Vec2 uvTranslate = Vec2(0.0f);
    Vec2 uvScale = Vec2(1.0f);
    float uvRotation = 0.0f;
    bool hasTextureAnimation = false;
};

// Data needed every frame for culling and drawing.
// Kept separate from ModelInstance (cold/debug data) to improve cache locality.
struct ModelInstanceHot {
    Mat4 transform = Mat4(1.0f);
    Vec3 worldAabbMin = Vec3(0.0f);
    Vec3 worldAabbMax = Vec3(0.0f);
    uint32_t meshIndex = 0;
    uint32_t flags = 0; // bit 0: enabled
    // Espelhado de ModelInstance::boundingRadius (Ofensor #1 do relatorio de
    // memoria). O culling de sombra lia isto do ModelInstance de 208B - agora
    // le daqui, do mesmo struct de 96->100B que o cull/draw da camera ja' usa.
    float boundingRadius = 0.0f;
};

// SoA layout used by the SIMD culling kernel. Input arrays must be 32-byte
// aligned and padded to a multiple of 8 elements.
struct alignas(32) ModelInstanceSoA {
    std::vector<float, AlignedAllocator<float, 32>> minX;
    std::vector<float, AlignedAllocator<float, 32>> minY;
    std::vector<float, AlignedAllocator<float, 32>> minZ;
    std::vector<float, AlignedAllocator<float, 32>> maxX;
    std::vector<float, AlignedAllocator<float, 32>> maxY;
    std::vector<float, AlignedAllocator<float, 32>> maxZ;
    std::vector<uint32_t, AlignedAllocator<uint32_t, 32>> meshIndex;
    std::vector<uint32_t, AlignedAllocator<uint32_t, 32>> flags;
    // Espelhado junto dos demais, para simetria com ModelInstanceHot. O kernel
    // AVX2 (cullAABBSoA) nao consome isto - so' testa AABB x frustum - mas o
    // filtro por tamanho projetado que roda DEPOIS do cull pode vir a usar
    // este array em vez de saltar para o Hot, se algum dia virar SIMD tambem.
    std::vector<float, AlignedAllocator<float, 32>> radius;

    void resize(uint32_t count) {
        minX.resize(count);
        minY.resize(count);
        minZ.resize(count);
        maxX.resize(count);
        maxY.resize(count);
        maxZ.resize(count);
        meshIndex.resize(count);
        flags.resize(count);
        radius.resize(count);
    }

    void clear() {
        minX.clear(); minY.clear(); minZ.clear();
        maxX.clear(); maxY.clear(); maxZ.clear();
        meshIndex.clear(); flags.clear(); radius.clear();
    }
};

class ModelRenderer {
    friend class Engine;
public:
    // Pisos do LOD de malha (ver m_meshLodMinTris / m_meshLodMinDistance).
    void setMeshLodFloors(uint32_t minTris, float minDistance, float farMinDistance = 0.0f) {
        m_meshLodMinTris = minTris;
        m_meshLodMinDistance = minDistance;
        m_meshLodFarMinDistance = farMinDistance;
    }

    // Triangulos submetidos no ultimo passe principal (pos-culling e pos-LOD) e
    // numero de draw calls. Alimentam a telemetria: sem isto, "GPU-bound em
    // geometria" vs "CPU-bound no dispatch" fica no chute.
    // Forca o nivel de LOD BASE no proximo render. O LOD e' escolhido por
    // distancia a camera, o que nao faz sentido num passe ortografico de mapa
    // inteiro: la' a "camera" e' um ponto arbitrario acima do centro e a
    // distancia nao tem relacao com o tamanho na tela.
    void setForceBaseLod(bool on) { m_forceBaseLod = on; }
    bool forceBaseLod() const { return m_forceBaseLod; }

    size_t lastVisibleCount() const { return m_visibleInstanceIndices.size(); }
    uint32_t lastDrawnTriangles() const { return m_lastDrawnTriangles; }
    uint32_t lastDrawCalls() const { return m_lastDrawCalls; }
    // "Cache hit/miss" do agrupamento por (malha,LOD) em sortVisibleByMesh -
    // ver o comentario la'. Reflete o ultimo passe que chamou o sort (cena,
    // sombra ou minimapa - o que rodou por ultimo nesse frame).
    uint32_t lastCacheHits() const { return m_lastCacheHits; }
    uint32_t lastCacheMisses() const { return m_lastCacheMisses; }

    bool init(VulkanContext* ctx, BindlessDescriptor* bindless);
    void shutdown();

    // Texture resolver: called to get bindless slot for a texture path.
    void setTextureResolver(std::function<uint32_t(const std::string&)> resolver) {
        m_textureResolver = std::move(resolver);
    }

    // Mesh resolver: called to get a GPU mesh. If it doesn't exist, the resolver creates it.
    void setMeshResolver(std::function<ModelMeshGPU(const std::string& key,
                                                     const std::vector<struct TerrainVertex>& vertices,
                                                     const std::vector<uint32_t>& indices)> resolver) {
        m_meshResolver = std::move(resolver);
    }

    // Load models from generic map data
    void loadMapModels(const std::vector<ModelAsset>& models,
                       const std::vector<ModelInstanceDesc>& instances,
                       float centerX, float centerZ);

    void clear();

    // Render all visible models to G-Buffer
    void render(VkCommandBuffer cmd,
                const Mat4& viewProj, const Frustum& frustum, const Vec3& cameraPos);

    // Banda perto do displacement híbrido: amplitude em unidades de mundo e
    // distância da banda (0 em qualquer um = desligado). O fade acontece nos
    // últimos 25% da banda, zerando exatamente na fronteira com o POM.
    // Distância (unidades de mundo) abaixo da qual o LOD fino é usado.
    // Histerese de 15% evita troca piscando quando a câmera para no limiar.
    void setGeoLodDistance(float d) { m_geoLodDistance = d; }

    // Configura o LOD grosseiro. ratio = fração de índices mantida na malha
    // simplificada (0 desliga); distanceFactor = múltiplos do raio da malha a
    // partir dos quais a versão simplificada é usada.
    void setMeshLod(float ratio, float error, float distanceFactor, uint32_t maxTris = 0,
                    uint32_t shadowMaxTris = 0) {
        m_meshLodRatio = ratio;
        m_meshLodError = error;
        m_meshLodDistanceFactor = distanceFactor;
        m_meshLodMaxTris = maxTris;
        m_shadowLodMaxTris = shadowMaxTris;
    }

    // LOD distante: teto de triangulos (0 desliga) e multiplo da distancia de
    // troca do "lo" a partir da qual entra.
    void setMeshLodFar(uint32_t farMaxTris, float farDistanceMul) {
        m_meshLodFarMaxTris = farMaxTris;
        m_meshLodFarDistanceMul = farDistanceMul;
    }
    // Orcamento de triangulos por pixel PROJETADO para a escolha de nivel
    // (ver lodSwitchFor). 0 desliga (so' o criterio por erro/distancia).
    void setMeshLodTrisPerPx(float trisPerPx) { m_meshLodTrisPerPx = std::max(0.0f, trisPerPx); }
    // Filtro de CHAO para renderShadow (G38, bake das probes): 0 = tudo,
    // 1 = so' malhas isGround, 2 = tudo menos isGround. Sem mexer na
    // assinatura de renderShadow. Voltar a 0 depois.
    // hugeXZ > 0: instancia com extensao X ou Z maior que isso conta como
    // chao (domo de ceu, plano d'agua, placa do mapa inteiro) - nao e' um
    // oclusor de luz do ceu, e' o proprio cenario.
    void setShadowGroundFilter(int filter, float hugeXZ = 0.0f) { m_shadowGroundFilter = filter; m_shadowGroundHugeXZ = hugeXZ; }
    // Diagnostico do bake: quantas malhas foram classificadas como chao.
    void groundMeshStats(uint32_t& ground, uint32_t& total, uint32_t& groundInstances) const {
        ground = total = groundInstances = 0;
        for (const auto& m : m_meshes) { ++total; if (m.isGround) ++ground; }
        for (const auto& hot : m_instanceHot)
            if ((hot.flags & 1u) && hot.meshIndex < m_meshes.size() && m_meshes[hot.meshIndex].isGround) ++groundInstances;
    }
    // AABB de mundo de todas as instancias habilitadas. false = nenhuma.
    bool sceneBounds(Vec3& outMin, Vec3& outMax) const {
        bool any = false;
        Vec3 mn(1e30f), mx(-1e30f);
        for (const auto& hot : m_instanceHot) {
            if (!(hot.flags & 1u)) continue;
            mn = glm::min(mn, hot.worldAabbMin);
            mx = glm::max(mx, hot.worldAabbMax);
            any = true;
        }
        if (any) { outMin = mn; outMax = mx; }
        return any;
    }
    // Descarte por tamanho na TELA: instancia cujo raio projetado fica abaixo
    // de minPixels nem entra na lista visivel. pixelsPerUnit = altura do
    // viewport / (2 tan(fov/2)) - pixels por unidade de mundo a distancia 1;
    // 0 desliga (minimapa, passes ortograficos).
    void setScreenCull(float minPixels) { m_screenCullMinPx = minPixels; }
    void setScreenMetrics(float pixelsPerUnit) { m_pixelsPerUnit = pixelsPerUnit; }
    // Caster de sombra: descarta quem projeta menos de minPx pixels NA TELA
    // (raio * pixelsPerUnit / distancia da camera). Criterio fisico e continuo
    // no zoom - substitui a curva artistica por zoom, que descartava props de
    // ate 10 u no zoom 0 (6 px de sombra sumindo de repente em cidade-A).
    void setShadowMinPx(float px) { m_shadowMinPx = px; }
    uint32_t lastScreenCulled() const { return m_lastScreenCulled; }

    // TESSELACAO DE HARDWARE NA BANDA PERTO. `bandDistance` > 0 liga o
    // caminho: malhas de CHAO cujo pedaco visivel cai dentro da banda sao
    // desenhadas com o pipeline de patch (model.tesc/model.tese) em vez do
    // pipeline normal. Todo o resto da cena continua no caminho de sempre.
    void setTessellation(bool enabled, float bandDistance) {
        m_tessEnabled = enabled;
        m_tessBandDistance = bandDistance;
    }
    bool tessellationActive() const { return m_tessEnabled && m_tessPipeline != VK_NULL_HANDLE; }

    void setGeoDisplacement(float amplitude, float distance) {
        m_geoDispAmplitude = amplitude;
        m_geoDispDistance = distance;
    }

    // Share the SpriteRenderer's FrameUBO descriptor (set 1) so the fragment
    // shader can read camera position, time and POM parameters. Must be called
    // after the SpriteRenderer has been initialized.
    void setFrameUboSet(VkDescriptorSetLayout layout, VkDescriptorSet set);

    // Recria a pipeline com a polygon mode atual de wireframeMode(). Chamador
    // (Engine::toggleWireframe) precisa garantir que a GPU esta ociosa antes -
    // destroi um VkPipeline que pode estar em uso por um comando em voo.
    void recreatePipeline();

    void setHighlightedInstance(int index) { m_highlightedInstanceIndex = index; }
    int getHighlightedInstance() const { return m_highlightedInstanceIndex; }

    // Render visible models to shadow map
    // windParams: mesmo vetor do FrameUBO (xyz=vento mundo, y reaproveitado
    // como swayScale, w=tempo do vento) - so' e' usado no caminho INSTANCIADO
    // (shadow_inst.vert), pra sombra de vegetacao acompanhar o balanco da
    // malha. Default 0 = sem vento (caminho por-draw nao aplica sway mesmo
    // assim, ver shadow.vert).
    void renderShadow(VkCommandBuffer cmd, VkPipeline shadowPipeline, VkPipelineLayout shadowLayout,
                      const Mat4& cascadeMatrix, const Frustum& shadowFrustum,
                      float minSizeThreshold, bool enableCulling,
                      bool instanced = false, const Vec4& windParams = Vec4(0.0f),
                      VkPipeline opaquePipeline = VK_NULL_HANDLE);

    bool isInitialized() const { return m_initialized; }
    void setMipmapsEnabled(bool enabled) { m_mipmapsEnabled = enabled; }
    size_t meshCount() const { return m_meshes.size(); }
    size_t instanceCount() const { return m_instances.size(); }
    // CENSO DE GEOMETRIA CARREGADA: triangulos da malha base somados por
    // instancia, ANTES de qualquer cull ou LOD. E' o numero que responde
    // "quanto do arquivo o loader guardou" - o que a telemetria reporta por
    // frame e' o SUBMETIDO (pos-cull, pos-LOD) e nao serve para isso; foi
    // confundir os dois que gerou o "San Miguel perde 20% no loader".
    uint64_t loadedTriangles() const {
        uint64_t t = 0;
        for (const auto& inst : m_instances) {
            if (inst.meshIndex < m_meshes.size()) t += m_meshes[inst.meshIndex].indexCount / 3;
        }
        return t;
    }

    // Animation support
    struct AnimatedNodeData {
        uint32_t modelAssetIndex = 0xFFFFFFFF;
        uint32_t nodeIndex = 0;
        Mat4 instanceMatrix = Mat4(1.0f);
        Mat4 mainOffset = Mat4(1.0f);
        uint32_t instanceIndex = 0;
    };
    std::vector<AnimatedNodeData> m_animatedNodes;

    // Per-asset animated node matrices, reused across frames (see
    // updateAnimations). `gen` marks the frame an entry was last recomputed.
    struct CachedAnimMatrices { std::vector<Mat4> matrices; uint64_t gen = 0; };
    std::unordered_map<uint32_t, CachedAnimMatrices> m_animMatrixCache;
    uint64_t m_animGen = 0;

    // Texture animation support (animated UVs: lava flow, rotating surfaces, etc.)
    struct TextureAnimatedNodeData {
        uint32_t modelAssetIndex = 0xFFFFFFFF;
        uint32_t nodeIndex = 0;
        uint32_t instanceIndex = 0;
    };
    std::vector<TextureAnimatedNodeData> m_textureAnimatedNodes;

    void updateAnimations(float totalElapsedMs);

    // Stored map data for reload
    std::vector<ModelAsset> m_lastModels;
    std::vector<ModelInstanceDesc> m_lastInstances;
    float m_lastCenterX = 0;
    float m_lastCenterZ = 0;
    void reloadModels();

    VkPipeline pipeline() const { return m_pipeline; }
    VkPipelineLayout pipelineLayout() const { return m_pipelineLayout; }

    const std::vector<ModelMeshGPU>& getMeshes() const { return m_meshes; }
    const std::vector<ModelInstance>& getInstances() const { return m_instances; }
    const std::unordered_map<std::string, uint32_t>& getTextureCache() const { return m_textureCache; }

    // Publico pro popup de preview (SpherePreview.cpp) conseguir achar o
    // MESMO MRAH-W que a malha real usaria pra essa textura, em vez de sempre
    // cair no chute por luminancia (era o bug: preview chamava setMaterial
    // com pbrIndex fixo em 0).
    PbrTextureSlots resolvePbrTextures(const std::string& path);
    // SO' LEITURA do cache (sem carregar/subir textura): seguro de chamar no
    // meio do frame (clique do F1). Toda textura usada por malha ja' foi
    // resolvida no load, entao pro popup de preview isto acha o MESMO MRAH-W
    // que o chao/parede real usa - sem isto o preview ficava no chute por
    // luminancia do shader (pbrIndex 0) e nunca batia com o mundo.
    PbrTextureSlots cachedPbrTextures(const std::string& path) const;
    // Mesmo PBR, achado pelo SLOT da textura (o que a lista do F1 tem).
    PbrTextureSlots pbrForTexSlot(uint32_t slot) const;

    // UV por unidade de mundo da primeira malha que usa essa textura (0 =
    // desconhecido). Pro popup de preview mostrar na escala do mundo.
    float uvPerUnitForSlot(uint32_t slot) const {
        auto it = m_texUvPerUnit.find(slot);
        return it != m_texUvPerUnit.end() ? it->second : 0.0f;
    }

private:
    VulkanContext* m_ctx = nullptr;
    BindlessDescriptor* m_bindless = nullptr;

    std::vector<ModelMeshGPU> m_meshes;
    std::vector<ModelInstance> m_instances;
    std::vector<uint32_t> m_textureSlots;

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_frameUboLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_frameUboSet = VK_NULL_HANDLE;

    std::unordered_map<std::string, uint32_t> m_textureCache;
    std::unordered_map<std::string, PbrTextureSlots> m_pbrTextureCache;
    std::unordered_map<uint32_t, float> m_texUvPerUnit;
    std::unordered_map<uint32_t, PbrTextureSlots> m_pbrByTexSlot;
    std::unordered_map<std::string, std::vector<uint32_t>> m_modelMeshCache;

    VkBuffer m_globalVertexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_globalVertexAlloc = VK_NULL_HANDLE;
    VkBuffer m_globalIndexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_globalIndexAlloc = VK_NULL_HANDLE;

    std::function<uint32_t(const std::string&)> m_textureResolver;
    std::function<ModelMeshGPU(const std::string&, const std::vector<struct TerrainVertex>&, const std::vector<uint32_t>&)> m_meshResolver;

    // Hot data used for culling and drawing; mirrors the subset of ModelInstance
    // fields touched in the render loop. m_instances remains the cold/authoritative
    // source for animation and debug access.
    std::vector<ModelInstanceHot> m_instanceHot;
    ModelInstanceSoA m_instanceSoA;
    std::vector<uint32_t> m_visibleInstanceIndices;
    // Scratch do agrupamento por malha (counting sort). Membros para não
    // alocar por frame; ver sortVisibleByMesh.
    std::vector<uint32_t> m_sortScratch;
    std::vector<uint32_t> m_meshHistogram;
    std::vector<uint32_t> m_shadowVisibleScratch;
    uint32_t m_shadowDrawCallsDbg = 0;
    uint32_t m_lastCacheHits = 0;
    uint32_t m_lastCacheMisses = 0;
    // useLod=false: agrupa so' por malha (passe de sombra desenha sempre o
    // index buffer da sombra, o LOD de camera era calculado - 2 sqrt por
    // instancia por cascata - e descartado; e ainda sobrescrevia
    // m_lodOfInstance, a histerese do passe principal). recordStats=false:
    // nao mexe em m_lastCacheHits/Misses (minimapa e cascatas sobrescreviam
    // o numero que a telemetria/F3 leem - achado do agente de cache 2026-09-02).
    void sortVisibleByMesh(std::vector<uint32_t>& list, bool useLod = true, bool recordStats = true);
    static constexpr uint32_t kLodLevels = 4;   // 0 = hi, 1 = cheio, 2 = lo, 3 = far
    // HISTERESE da troca cheio<->reduzido: sobe pra reduzido em
    // loSwitchDistance, so' volta pra cheio abaixo de loSwitchDistance*ESTE
    // fator. O intervalo [d*fator, d] e' onde os DOIS niveis sao possiveis
    // dependendo da direcao do movimento - e e' EXATAMENTE a banda em que o
    // crossfade desenha os dois (ver render()). Os dois lugares TEM que usar
    // esta constante: com bandas diferentes aparece um degrau duro na borda
    // onde uma acaba e a outra nao comecou (jitter relatado pelo autor ao
    // varrer o pitch no zoom 0, 2026-09-03).
    static constexpr float kLoLodHysteresis = 0.85f;
    // Mesmos pares para as outras duas fronteiras. hi e' "mais perto que", por
    // isso o fator dele e' > 1 (a banda abre para FORA do limiar).
    static constexpr float kHiLodHysteresis = 1.15f;
    static constexpr float kFarLodHysteresis = 0.85f;

    // Par de niveis que uma instancia esta' atravessando AGORA, se estiver.
    // `t` = 0 no nivel `nearLevel` inteiro, 1 no `farLevel` inteiro.
    struct LodFadePair {
        bool active = false;
        float t = 0.0f;
        uint8_t nearLevel = 1;  // menor indice = usado mais PERTO da camera
        uint8_t farLevel = 2;
    };
    // Qual fronteira (hi<->cheio, cheio<->reduzido, reduzido<->far) esta'
    // sendo atravessada nesta distancia. Uma so' por vez: se duas bandas se
    // sobrepuserem num mesh mal configurado, vence a primeira na ordem de
    // distancia crescente - degrada para um crossfade so', nunca para dois.
    // DISTANCIAS DE TROCA EFETIVAS de uma instancia. Partem das distancias por
    // malha (erro geometrico) e, com m_meshLodTrisPerPx > 0, sao PUXADAS PARA
    // PERTO ate' onde o nivel ainda cabe no orcamento de triangulos por pixel
    // projetado: o nivel L com T_L triangulos e' aceitavel ate'
    //     d_L = R * pxUnit * sqrt(pi * orcamento / T_L)
    // (R = raio da instancia no mundo). Medido no parana_demo a pitch 30:
    // 14 M triangulos rasterizados para 2,7 M fragmentos - ~5 triangulos por
    // FRAGMENTO. Triangulo subpixel nao e' detalhe, e' custo de vertice puro;
    // o criterio por erro nao enxerga isso porque o erro e' em unidades de
    // mundo, nao em pixels. Nunca abaixo do piso m_meshLodMinDistance, e o far
    // fica >= 1,2x o lo para as bandas de fade nao se cruzarem.
    struct LodSwitch { float lo = 0.0f; float far = 0.0f; };
    LodSwitch lodSwitchFor(const ModelInstanceHot& hot, const ModelMeshGPU& mesh) const;
    static LodFadePair lodFadePairFor(const ModelMeshGPU& mesh, const LodSwitch& sw, float dist);
    uint8_t lodLevelFor(uint32_t idx, const ModelMeshGPU& mesh) const;
    std::vector<uint8_t> m_lodOfInstance;   // preenchido pelo sort, lido no draw
    Vec3 m_lastCameraPos = Vec3(0.0f);

    // ---- Instancing (SIMT) ----
    // Espelho C++ do EngInstance do model.vert (std430). NAO reordenar.
    struct GpuModelInstance {
        Mat4 model;
        Vec4 uvTranslateRot;
        Vec4 uvScaleDisp;
    };
    // Um buffer por frame-em-voo: o frame N-2 ainda LE o dele enquanto este
    // escreve (o padrao de buffer unico do SpriteRenderer tem corrida).
    static constexpr uint32_t kInstFrames = 3; // = VulkanContext::MAX_FRAMES_IN_FLIGHT
    VkDescriptorSetLayout m_instLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_instPool = VK_NULL_HANDLE;
    VkDescriptorSet m_instSets[kInstFrames] = {};
    VkBuffer m_instBuffers[kInstFrames] = {};
    VmaAllocation m_instAllocs[kInstFrames] = {};
    void* m_instMapped[kInstFrames] = {};
    uint32_t m_instCapacity = 0;
    // render() roda MAIS DE UMA VEZ por frame (cena + minimapa, Engine.cpp:8434).
    // Sem cursor, a segunda chamada sobrescrevia as instancias da primeira e a
    // GPU - que executa depois - lia os dados errados nas duas. Reportado como
    // "o minimap ta sendo alterado com o culling".
    // Nos efetivamente consultados por asset (anim.nodeIndex), para podar a
    // travessia da hierarquia em computeAnimatedRenderMatrices.
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_animQueriedNodes;
    uint32_t m_instCursor = 0;
    uint32_t m_instLastFrame = 0xFFFFFFFFu;
    bool ensureInstanceCapacity(uint32_t count);
    void destroyInstanceBuffers(bool deferred);
    // Sombra instanciada: um cursor de append por frame (cada cascata escreve
    // seu trecho no mesmo buffer; reseta quando o frame vira). mat4 por caster.
    VkDescriptorSet m_shadowSets[kInstFrames] = {};
    VkBuffer m_shadowBuffers[kInstFrames] = {};
    VmaAllocation m_shadowAllocs[kInstFrames] = {};
    void* m_shadowMapped[kInstFrames] = {};
    uint32_t m_shadowCapacity = 0;
    uint32_t m_shadowCursor = 0;
    uint32_t m_shadowLastFrame = 0xFFFFFFFFu;
    bool ensureInstPool();
    bool ensureShadowCapacity();
    void destroyShadowBuffers(bool deferred);
    std::vector<uint8_t> m_visibleMaskScratch; // reused per-frame cull mask
    // Mascara do cull de SOMBRA, separada da da camera (m_visibleMaskScratch):
    // renderShadow roda 2-3x por frame (cascatas + chuva) com frustums
    // diferentes, e nao deve pisar no resultado que o cull da camera possa
    // ainda precisar no mesmo frame.
    std::vector<uint8_t> m_shadowMaskScratch;
    // Ofensor #1 do relatorio de memoria: antes, o cull e o draw da sombra
    // escaneavam m_instances (AoS de 208B) do zero a cada chamada. Agora usam
    // ModelInstanceSoA/ModelInstanceHot, os MESMOS structs que o cull/draw da
    // camera ja' usava - so' precisavam estar atualizados quando a sombra
    // roda primeiro no frame. Este flag garante que o rebuild (Hot->SoA)
    // acontece UMA vez por frame, nao importa se camera ou sombra pede primeiro.
    bool m_instanceSoADirty = true;
    void ensureInstanceSoAFresh();
    float m_geoLodDistance = 0.0f;
    std::vector<uint8_t> m_hiLodActive; // histerese por instância
    // LOD grosseiro de malha (meshoptimizer). ratio 0 = desligado.
    static constexpr size_t kMeshLodMinIndices = 300; // piso absoluto de seguranca
    // Piso REAL de triangulos para gerar LOD, e piso de distancia de troca.
    // Motivo (reportado pelo autor: "pop esquisito nos postes de cidade-A"):
    // um poste tem ~200-500 triangulos e extensao pequena, entao (a) o LOD de
    // 10% o reduz a 20-50 triangulos, destruindo a forma, e (b) a distancia de
    // troca, que vem de erro_geometrico * extensao, sai minuscula - ele troca
    // quase colado na camera e pipoca quando ela se move. Malha pequena nao
    // paga o passe: simplifica-la e' perda pura.
    uint32_t m_meshLodMinTris = 800;
    float m_meshLodMinDistance = 0.0f;
    // Piso da troca pro nivel "far" (cartoes 8x): o orcamento tri/px puxa o
    // far pra ~2x o lo, e com camera de diorama (700+ u) isso poe lencois em
    // meia tela. 0 = so' o piso do lo x 1,2.
    float m_meshLodFarMinDistance = 0.0f;
    float m_meshLodRatio = 0.0f;
    float m_meshLodError = 0.05f;
    float m_meshLodDistanceFactor = 8.0f;
    // Teto absoluto de triangulos do LOD. 0 = so' a fracao.
    uint32_t m_meshLodMaxTris = 0;
    uint32_t m_shadowLodMaxTris = 0;
    uint32_t m_meshLodFarMaxTris = 0;
    float m_meshLodFarDistanceMul = 3.0f;
    float m_meshLodTrisPerPx = 0.0f;   // ver setMeshLodTrisPerPx / lodSwitchFor
    int m_shadowGroundFilter = 0;      // ver setShadowGroundFilter
    float m_shadowGroundHugeXZ = 0.0f;
    float m_screenCullMinPx = 0.0f;
    float m_shadowMinPx = 0.0f;
    // Histerese do descarte de caster por tamanho na tela: sem ela o objeto
    // que cruza o limiar liga e desliga a sombra a cada frame com a camera em
    // movimento, e o pop e' bem visivel (relatado pelo autor 2026-09-03).
    std::vector<uint8_t> m_shadowCulledPrev;
    float m_pixelsPerUnit = 0.0f;
    uint32_t m_lastScreenCulled = 0;
    std::vector<uint8_t> m_screenCulledPrev; // histerese do descarte por tela
    // Ordenacao frente-para-tras dos baldes (early-Z): um registro por balde
    // nao-vazio, reusado entre frames para nao alocar no caminho quente.
    struct BucketRange { float dist2; uint32_t first; uint32_t last; };
    std::vector<BucketRange> m_bucketOrder;
    std::vector<uint8_t> m_loLodActive;
    // UV por instancia ESPELHADA (paralela a m_instanceHot, mesma sincronia
    // via rebuildHotData). O laco de desenho lia isto do ModelInstance, que
    // e' o struct FRIO de 208 B: so' pra pegar 24 B de UV puxava 2-4 linhas de
    // cache por instancia visivel, e o resto do struct (nome, caminho do
    // asset, AABB duplicada...) ia junto sem ser usado. Fica FORA do
    // ModelInstanceHot de proposito: o kernel AVX2 de culling varre aquele
    // array inteiro e nao usa UV nenhuma - engordar o Hot pioraria o cull.
    // Ja' vem com os mesmos defaults que o laco escrevia (sem animacao:
    // translate 0, rotacao 0, escala 1).
    std::vector<Vec4> m_instUvTranslateRot;
    std::vector<Vec2> m_instUvScale;
    // ERUPTION_DEBUG_LODPOP=1: cobertura efetiva da instancia no frame
    // ANTERIOR (1.0 = nivel inteiro; entre 0 e 1 = fracao do crossfade).
    // Serve para contar quantas instancias PULAM a cobertura de um frame pro
    // outro - um degrau de LOD e' exatamente isso, e e' o que o olho le como
    // "jitter". So' e' alocado quando o debug esta ligado.
    // DISTANCIA SUAVIZADA PARA O LOD, por instancia. A banda de crossfade e'
    // por distancia (0,85 x troca -> troca, 37,5 u para a troca de 250) e nao
    // tinha componente de tempo: um scroll de zoom (m_orbitDistance /= 1,1,
    // instantaneo) anda dezenas de unidades num frame e pula a banda inteira,
    // e o modo tatico salta 300 u. Medido com dolly a 19 u/frame: 508 pops e
    // 81 trocas duras em 49 frames. Esta distancia segue a real, mas DENTRO
    // de uma banda anda no maximo largura/kLodFadeMinFrames por frame; fora
    // das bandas salta. Todo consumidor de LOD (nivel, pre-passe de fade,
    // draw) le' daqui, entao os tres concordam.
    std::vector<float> m_lodDistShown;
    std::vector<uint32_t> m_lodDistFrame;  // frame em que foi atualizada; != anterior -> reinicia
    uint32_t m_lodFrame = 0;
    float advanceLodDistance(uint32_t idx, const ModelInstanceHot& hot, const ModelMeshGPU& mesh);
    float lodDistanceFor(uint32_t idx, const ModelInstanceHot& hot) const;
    std::vector<float> m_lodCoveragePrev;
    // Frame em que a instancia foi vista pela ULTIMA vez no passe principal.
    // Sem isto o contador acusa "pop" quando a instancia so' ENTROU no campo
    // de visao (a cobertura anterior era de um frame antigo, ou nem existia) -
    // durante uma varredura de pitch entram centenas de instancias, e o
    // numero vira ruido.
    std::vector<uint32_t> m_lodSeenFrame;

    bool m_tessEnabled = false;
    float m_tessBandDistance = 0.0f;
    VkPipeline m_tessPipeline = VK_NULL_HANDLE;
    float m_geoDispAmplitude = 0.0f;
    float m_geoDispDistance = 0.0f;

    // Pad SoA vectors so the AVX2 kernel always sees a multiple of 8 elements.
    uint32_t m_paddedInstanceCount = 0;

    bool m_mipmapsEnabled = true;
    bool m_initialized = false;
    int m_highlightedInstanceIndex = -1;

    void createPipeline();
    void rebuildHotData();
    void rebuildInstanceSoA();
    void cullInstances(const Frustum& frustum);
    void uploadMesh(const std::vector<struct TerrainVertex>& vertices,
                    const std::vector<uint32_t>& indices,
                    ModelMeshGPU& mesh);
    uint32_t resolveTexture(const std::string& path);
    // So' registra (nome ja' sanitizado -> slot) em m_textureCache, sem
    // chamar o resolver - pros dois ramos de loadMapModels que ACHAM o slot
    // de outro jeito (mapa de indice global do formato v1.x-2.2, ou fallback
    // por indice direto) e nunca passavam por resolveTexture(). Sem isto
    // aquelas texturas nunca apareciam em getTextureCache() - nem no popup
    // de preview (F1), nem em nenhum ERUPTION_TEST_SPHERE_PREVIEW: eram
    // exatamente as texturas de CHAO de mapas mais antigos (autor: "o chao
    // continua diferente do preview" - a textura real do chao nem aparecia
    // pra' clicar).
    void registerResolvedTexture(const std::string& path, uint32_t slot);

    bool m_forceBaseLod = false;
    uint32_t m_lastDrawnTriangles = 0;
    uint32_t m_lastDrawCalls = 0;
};

} // namespace eruption
