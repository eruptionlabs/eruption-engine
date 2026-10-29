#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "renderer/MipmapGenerator.hpp"
#include "renderer/ModelData.hpp"
#include "utils/MeshLod.hpp"
#include "utils/PbrTextureLoader.hpp"
#include "utils/PbrMaterialProfile.hpp"
#include "math/Frustum.hpp"
#include "math/CullingSIMD.hpp"
#include "utils/MeshClusters.hpp"
#include "utils/AlignedAllocator.hpp"
#include "renderer/RaycastBvh.hpp"

#include <atomic>
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <chrono>
#include <future>
#include <mutex>
#include <cstdint>

namespace eruption {

struct ReliefFloorGrid;

struct ModelMeshGPU {
    // Solda da costura do deslocamento (ModelRenderer::createSeamWeld): textura
    // de dados por triangulo do indice base (model_tese.glsl, weldTexel). E' do
    // ModelRenderer, como os niveis de LOD: nasce depois do resolver.
    uint32_t weldSlot = 0, weldRows = 0, weldTris = 0;
    VkImage weldImage = VK_NULL_HANDLE;
    VkImageView weldView = VK_NULL_HANDLE;
    VmaAllocation weldAlloc = VK_NULL_HANDLE;
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    // Malha com esqueleto: posições do frame anterior (vetor de movimento);
    // NULL = a própria malha (vértice parado). Dono: o skinning.
    VkBuffer prevVertexBuffer = VK_NULL_HANDLE;
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

    // CADEIA INTERMEDIARIA (niveis 4..6): malha gigante entre a cheia e o "lo",
    // cada degrau com ~1/4 dos triangulos do anterior. Motivo medido no
    // parana_field original: arvores de 1,6 M e 1,07 M de triangulos saltavam
    // da cheia direto para o "lo" de 10 k; as ~40 mais perto desenhavam a
    // malha cheia = 28 dos 29,5 M de triangulos do frame (G-buffer 50 ms na
    // 4060). A escolha e' por triangulo por pixel projetado (o mesmo
    // orcamento de lodSwitchFor): de perto continua a malha cheia.
    // PEDACOS de um nivel pesado: os indices vao em ordem de grade 3D local
    // (Morton) e cada pedaco guarda a caixa dos proprios triangulos. Instancia
    // cortada pela borda da tela desenha so' os pedacos que tocam a visao.
    using Cluster = MeshCluster;
    struct MidLod {
        VkBuffer vertexBuffer = VK_NULL_HANDLE; // cartoes podados; NULL = o da malha
        VmaAllocation vertexAlloc = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation indexAlloc = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        std::vector<Cluster> clusters;
    };
    static constexpr uint32_t kMaxMidLods = 3;
    MidLod mid[kMaxMidLods];
    uint32_t midCount = 0;

    // LOD EXCLUSIVO DA SOMBRA, bem mais agressivo que o da tela. Medido: trocar
    // a malha da sombra pelo LOD dá RMS 0,000 - a silhueta simplificada projeta
    // nos MESMOS texels do shadow map, que é bem mais grosseiro que a tela.
    // Então a sombra pode pagar um LOD que seria inaceitável no passe visível.
    VkBuffer shIndexBuffer = VK_NULL_HANDLE;
    VmaAllocation shIndexAlloc = VK_NULL_HANDLE;
    uint32_t shIndexCount = 0;

    // RELEVO ASSADO NA CARGA (preset "relief_bake", ModelRenderer::applyReliefs):
    // vertex buffer = os vertices da malha ja' deslocados, nos MESMOS indices,
    // e depois os vertices novos da subdivisao - todo nivel de LOD que usa os
    // vertices da malha (sombra, "lo", celulas do chao) desenha o relevo sem
    // indice novo. Index buffer = o que a malha subiu (cheia + niveis das
    // celulas) e, no fim, a malha cheia partida, a partir de reliefFirst.
    // reliefCells = groundCells com first/count na parte partida. Dono: o
    // ModelRenderer (destroyLodBuffers), mesmo com a malha externa.
    // reliefInView: o G-buffer tambem usa (celular, sem tesselacao); senao so'
    // a sombra (PC: a tesselacao segue no G-buffer e o sol ve' o relevo).
    VkBuffer reliefVertexBuffer = VK_NULL_HANDLE;
    VmaAllocation reliefVertexAlloc = VK_NULL_HANDLE;
    VkBuffer reliefIndexBuffer = VK_NULL_HANDLE;
    VmaAllocation reliefIndexAlloc = VK_NULL_HANDLE;
    uint32_t reliefFirst = 0, reliefIndexCount = 0;
    bool reliefInView = false;
    std::vector<GroundCell> reliefCells;
    // Chao com a malha assada na tela: quanto a superficie subiu (ver
    // ModelRenderer::reliefFloorDelta). nullptr = o chao de antes vale.
    std::shared_ptr<const ReliefFloorGrid> reliefFloor;

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
    // Recorte por alfa SEM splat nem chao: entra no pre-passe de profundidade
    // do G-buffer (ver ModelRenderer::render e model_depth.frag).
    bool depthPrepass = false;

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
    bool viewHidden = false; // escondido só na vista do editor (não é apagar)
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
    // Vista de detalhe total (editor): a sombra também usa a malha original.
    void setShadowFullDetail(bool on) { m_shadowFullDetail = on; }
    bool forceBaseLod() const { return m_forceBaseLod; }

    size_t lastVisibleCount() const { return m_visibleInstanceIndices.size(); }
    uint32_t lastDrawnTriangles() const { return m_lastDrawnTriangles; }
    uint32_t lastDrawCalls() const { return m_lastDrawCalls; }
    // "Cache hit/miss" do agrupamento por (malha,LOD) em sortVisibleByMesh -
    // ver o comentario la'. Reflete o ultimo passe que chamou o sort (cena,
    // sombra ou minimapa - o que rodou por ultimo nesse frame).
    uint32_t lastCacheHits() const { return m_lastCacheHits; }
    // POP na tela, do ultimo passe principal (sortVisibleByMesh): instancias a
    // 90% do centro da tela que trocaram de nivel de LOD, que apareceram e que
    // sumiram desde o quadro anterior. A borda fica de fora: la' a peca entra
    // e sai pelo enquadramento, o que nao e' pop.
    // popInWhy: por que a peca que apareceu estava fora no quadro anterior
    // (0 frustum, 1 desligada, 2 pequena na tela, 3 raleamento).
    struct PopStats { uint32_t lodSwaps = 0, popIn = 0, popOut = 0; uint32_t popInWhy[4] = {}; uint32_t popInNew = 0; };
    const PopStats& lastPopStats() const { return m_popStats; }
    // ENTRADA SEM POP: instancia que surge depois do primeiro quadro do mapa
    // (spawn por script, scatter, modelo carregado no meio do jogo) ou que
    // estava desligada e volta, entra em dither de 0 a 100% em
    // ERUPTION_SPAWN_FADE segundos (padrao 0,3; 0 desliga), no relogio de
    // setSpawnFadeClock (o do jogo: passo fixo nos testes). skipSpawnFade: a
    // proxima entrada desta instancia aparece inteira.
    // Tela coberta (cortina de fx.fade): o que nasce por baixo dela entra
    // inteiro - a cortina ja' e' a transicao, e dissolver depois dela abrir
    // mostraria o palco se montando.
    void setSpawnFadeSuspended(bool on) { m_spawnFadeSuspended = on; }
    void setSpawnFadeClock(float seconds) { m_spawnClock = seconds; }
    void skipSpawnFade(uint32_t index) {
        if (index >= m_spawnNoFade.size()) m_spawnNoFade.resize(index + 1, 0);
        m_spawnNoFade[index] = 1;
    }
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

    // Load models from generic map data. Fica com `models` (movido, sem
    // cópia): é a cópia que o editor usa depois para criar instâncias.
    void loadMapModels(std::vector<ModelAsset>&& models,
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

    // Liga/desliga a geração de LOD das malhas criadas daqui em diante (o chão
    // de um mapa vizinho entra sem: simplificar na thread principal travava o frame).
    void setMeshLodSuspended(bool suspended) {
        if (suspended && m_meshLodRatio > 0.0f) { m_meshLodSaved = m_meshLodRatio; m_meshLodRatio = 0.0f; }
        else if (!suspended && m_meshLodSaved > 0.0f) { m_meshLodRatio = m_meshLodSaved; m_meshLodSaved = 0.0f; }
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
    // Pixels RENDERIZADOS / pixels da tela (render scale, FSR). As metricas de
    // tela ficam na resolucao final (descarte por tamanho), mas o orcamento de
    // triangulos segue o que o rasterizador de fato desenha: com o FSR em
    // qualidade sao 44% dos pixels, e o mesmo triangulo por pixel de antes.
    void setLodPixelScale(float s) { m_lodPixelScale = std::clamp(s, 0.05f, 1.0f); }
    // TETO GLOBAL de triangulos por frame (passe principal). O orcamento por
    // pixel acima e' por OBJETO: dez arvores de 1,6 M que cobrem a tela, cada
    // uma, pedem a malha cheia e somam 16 M. Com o teto, a escala do orcamento
    // por pixel cai (suave, alguns frames) ate' o total caber: o detalhe vai
    // para quem ocupa mais tela por triangulo. 0 = sem teto.
    void setMeshTriBudget(uint32_t tris) { m_meshTriBudget = tris; }
    float lodBudgetScale() const { return m_lodBudgetScale; }
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
    void setScreenMetrics(float pixelsPerUnit, float screenW = 0.0f, float screenH = 0.0f) {
        m_pixelsPerUnit = pixelsPerUnit;
        m_screenW = screenW;
        m_screenH = screenH;
    }
    // Caster de sombra: descarta quem projeta menos de minPx pixels NA TELA
    // (raio * pixelsPerUnit / distancia da camera). Criterio fisico e continuo
    // no zoom - substitui a curva artistica por zoom, que descartava props de
    // ate 10 u no zoom 0 (6 px de sombra sumindo de repente em cidade-A).
    void setShadowMinPx(float px) { m_shadowMinPx = px; }
    uint32_t lastScreenCulled() const { return m_lastScreenCulled; }
    // Casters da ultima cascata cuja sombra cai toda fora da camera.
    uint32_t lastShadowReceiverCulled() const { return m_lastShadowReceiverCulled; }

    // TESSELACAO DE HARDWARE NA BANDA PERTO. `bandDistance` > 0 liga o
    // caminho: malhas de CHAO cujo pedaco visivel cai dentro da banda sao
    // desenhadas com o pipeline de patch (model.tesc/model.tese) em vez do
    // pipeline normal. Todo o resto da cena continua no caminho de sempre.
    void setTessellation(bool enabled, float bandDistance) {
        m_tessEnabled = enabled;
        m_tessBandDistance = bandDistance;
    }
    bool tessellationActive() const { return m_tessEnabled && m_tessPipeline != VK_NULL_HANDLE; }
    float tessBandDistance() const { return m_tessBandDistance; }
    // Amplitude do deslocamento (u_tessParams.z) para o teto por tamanho de
    // malha (reliefSizeScale em ModelRenderer.cpp).
    void setTessAmplitude(float amplitude) { m_tessAmplitude = amplitude; }
    // Relevo assado na carga (data/graphics.json "relief_bake*", aplicado em
    // Engine::applyGraphicsPreset): vale para as malhas carregadas depois.
    // mode 0 = nao assa; 1 = so' a sombra; 2 = sombra e G-buffer (sem tesselacao).
    struct ReliefBakeConfig {
        // 0 desliga, 1 so' a sombra, 2 G-buffer e sombra sem tesselacao
        // nenhuma (celular), 3 G-buffer e sombra com a tesselacao so' nas
        // malhas sem assado (PC). No 3 a malha assada vai pelo pipeline
        // normal e sem o deslocamento do model.vert: a altura da tesselacao
        // dependia da camera (amplitude pela curva de distancia, mip da altura
        // pelo piso em pixels, grade pela distancia) e o chao entortava ao
        // girar e ao dar zoom.
        int mode = 0;
        float spacing = 2.0f;   // grade (u)
        float amplitude = 2.5f; // u, igual para toda malha (relief_bake_amplitude)
        float gain = 1.0f;      // contraste da altura em volta da media (relief_bake_height_gain)
        uint32_t maxTriangles = 600000; // alvo por malha (a grade abre acima disto)
    };
    void setReliefBake(const ReliefBakeConfig& config) { m_reliefBake = config; }
    const ReliefBakeConfig& reliefBake() const { return m_reliefBake; }
    // Altura crua de uma textura pelo caminho dela: identidade do conteudo e,
    // com pixels = true, o alfa (o Engine decodifica o MRAH-W do bake das
    // texturas embutidas). Chamada tambem das threads de trabalho da carga.
    // Ver renderer/ReliefSource.
    using ReliefTextureSource = std::function<bool(const std::string& texture, bool pixels, std::vector<uint8_t>& alpha,
                                                   int& w, int& h, uint64_t& id)>;
    void setReliefSource(ReliefTextureSource source) { m_reliefSource = std::move(source); }
    // Quanto o chao assado na tela subiu em (x, z) do mundo, na malha cuja
    // superficie de antes passa a ate' 1 u de baseY (0 fora dela ou sem assado
    // na tela): somado ao piso de antes (Engine::sampleTerrainHeight, floorHeight)
    // para o pe' e o caminho pisarem no que se ve'. Seguro em qualquer thread.
    float reliefFloorDelta(float x, float z, float baseY) const;
    // Solda da costura (preset "tessellation_seam_weld"): vale para as malhas
    // carregadas depois da chamada.
    void setSeamWeld(bool enabled) { m_seamWeld = enabled; }

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
                      VkPipeline opaquePipeline = VK_NULL_HANDLE,
                      const Frustum* receivers = nullptr, float receiverMargin = 0.0f);

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
        // mainOffset * matriz animada do último frame: mover a instância
        // (editor, cena, script) recalcula instanceMatrix a partir dela.
        Mat4 lastLocal = Mat4(1.0f);
    };
    std::vector<AnimatedNodeData> m_animatedNodes;
    std::unordered_map<uint32_t, size_t> m_animByInstance; // instância -> m_animatedNodes
    size_t m_animByInstanceCount = SIZE_MAX;
    size_t m_animQueriedCount = 0;
    int animIndexOf(uint32_t instance);
    void copyAnimatedNode(uint32_t source, uint32_t copy);

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
    // Edicao (editor): troca a matriz ou a visibilidade de uma instancia e
    // refaz a AABB de mundo e o espelho quente usados pelo culling.
    // `fromOwner`: quem escreve e' o objeto da cena dono da instancia (nao
    // entra na lista de mexidas por fora, senao o objeto puxaria de volta).
    void setInstanceTransform(uint32_t index, const Mat4& transform, bool fromOwner = false);
    void setInstanceEnabled(uint32_t index, bool enabled, bool fromOwner = false);
    // Instancias mexidas por fora desde a ultima chamada (script, editor, jogo
    // salvo, terreno): os objetos da cena puxam dai' o que mudou.
    void takeTouchedInstances(std::vector<uint32_t>& out);
    // Lugar da instância sem a animação rígida do nó (a que anda sozinha a
    // cada quadro fica por cima). Para instância sem animação é a matriz dela.
    // Os objetos da cena guardam e mexem este lugar: a pose não entra na conta.
    Mat4 instancePlacement(uint32_t index);
    void setInstancePlacement(uint32_t index, const Mat4& placement, bool fromOwner = false);
    // Tira a instância da lista de mexidas (quem mexeu já é o dono dela).
    void forgetTouch(uint32_t index);
    // Muda quando a lista de instancias e' refeita do zero (mapa novo).
    uint32_t instanceEpoch() const { return m_instanceEpoch; }
    // Chao com splat do mapa aberto (a maior malha de chao com mascara), para a
    // juncao malha x terreno (Engine::fillGroundBlend): slots das camadas, das
    // mascaras e do material base, e a UV e a mascara como funcao afim do XZ do
    // mundo (u = a*x + b*z + c). valid = false sem chao com splat.
    struct GroundBlendInfo {
        bool valid = false;
        uint32_t splat[4] = {};               // splatTex01, 23, 45, 67
        uint32_t mask = 0, mask2 = 0;         // mascaras das camadas 0-3 e 4-7
        uint32_t color = 0, normal = 0, pbr = 0; // material base do chao
        Vec3 uvU{0.0f}, uvV{0.0f}, maskU{0.0f}, maskV{0.0f};
        size_t triangles = 0;
        uint32_t mesh = 0xFFFFFFFFu;          // a malha de chao de onde veio
        // Altura da PROPRIA malha quando ela e' uma grade regular (nx x nz
        // vertices em x0 + i*dx, z0 + j*dz, no mundo): a referencia certa da
        // juncao. Vazia = sem grade (a juncao usa a do .ter).
        std::vector<float> heights;
        uint32_t nx = 0, nz = 0;
        float x0 = 0.0f, z0 = 0.0f, dx = 1.0f, dz = 1.0f;
    };
    // Captura PREGUICOSA: o splat so' marca o chao (patchGroundVertices); o
    // ajuste roda aqui, no primeiro quadro com a juncao ligada. `wantHeights`:
    // monta tambem a grade de altura da malha (so' sem .ter).
    const GroundBlendInfo& groundBlend(bool wantHeights);
    void setInstanceName(uint32_t index, const std::string& name) {
        if (index < m_instances.size()) m_instances[index].name = name;
    }
    // Cópia de uma instância (mesma malha) com outro nome e matriz; devolve o
    // índice novo. Os vetores por instância se ajustam sozinhos ao tamanho.
    uint32_t duplicateInstance(uint32_t source, const std::string& name, const Mat4& transform);
    // Acrescenta um modelo ao mapa aberto (editor: arrastar um .glb para a
    // cena). Devolve o indice da primeira instancia criada (uma por no) ou
    // 0xFFFFFFFF se o modelo nao tinha malha.
    // `nodeName` vazio = todos os nós; senão só o nó com esse nome.
    uint32_t addModel(const ModelAsset& asset, const ModelInstanceDesc& ref, const std::string& nodeName = {});
    uint32_t addModel(ModelAsset&& asset, const ModelInstanceDesc& ref, const std::string& nodeName = {});
    // Libera as malhas de um arquivo (mapa vizinho que saiu do mundo): as instâncias
    // delas param de desenhar. Devolve as chaves ("arquivo#nó") das malhas base,
    // que pertencem ao cache de assets do motor.
    std::vector<std::string> releaseAsset(const std::string& filePath);
    // Pé do objeto no chão (XZ do mundo, 4 cantos no sentido anti-horário): a
    // caixa dos vértices da parte de baixo da malha (tronco, não a copa).
    bool footprint(uint32_t index, Vec2 out[4]) const;
    // Mesmo que addModel para um arquivo já carregado (sem reler o .glb).
    bool hasAsset(const std::string& path) const;
    uint32_t addInstanceOf(const std::string& path, const ModelInstanceDesc& ref, const std::string& nodeName = {});

    // Montagem de malha fora da thread principal (mapa vizinho do mundo
    // contínuo). scanSlotUse e prepareAsset rodam em qualquer thread;
    // resolveSlots, lodParams, providePrepared e o addModel seguinte, na
    // principal. Sem providePrepared o addModel monta tudo ali mesmo.
    struct NodeSlotUse {
        std::vector<int32_t> faceFirst;   // por entrada: o índice de textura que a resolve (-1 = sem uso)
        std::vector<uint32_t> faceOrder;  // entradas na ordem em que aparecem nos vértices
        std::vector<uint8_t> blend, mask, splat;
    };
    struct NodeSlots {
        struct Face { uint32_t texSlot = 0, pbrIdx = 0, normalIdx = 0, emissiveSlot = 0; };
        struct Blend { uint32_t tex = 0, pbrIdx = 0, normalIdx = 0; };
        std::vector<Face> face;
        std::vector<Blend> blend;
        std::vector<uint32_t> mask, splat;
        std::string firstTexName, firstPbrName, material;
        PbrMaterialProfile profile;
    };
    // Parâmetros do LOD copiados quando a tarefa nasce: o cálculo não lê membros.
    struct LodParams {
        float ratio = 0.0f, error = 0.05f, distanceFactor = 8.0f, minDistance = 0.0f, farDistanceMul = 3.0f;
        uint32_t maxTris = 0, farMaxTris = 0, shadowMaxTris = 0, minTris = 800;
    };
    struct PreparedAsset;
    static std::vector<NodeSlotUse> scanSlotUse(const ModelAsset& asset);
    std::vector<NodeSlots> resolveSlots(const ModelAsset& asset, const std::vector<NodeSlotUse>& use);
    LodParams lodParams() const;
    static PreparedAsset prepareAsset(const ModelAsset& asset, std::vector<NodeSlots> slots, const LodParams& lod);
    void providePrepared(const std::string& filePath, PreparedAsset&& prepared);
    // Entrada de vizinho no mundo continuo: o relevo assa em segundo plano e
    // entra num quadro seguinte (pollReliefJobs, uma vez por quadro).
    void setReliefDeferred(bool deferred) { m_reliefDeferred = deferred; }
    void pollReliefJobs();
    // Distancia ao longo do raio (mundo, dir normalizada) ate' o triangulo
    // mais proximo da instancia, ou -1. Usa a malha da CPU do modelo.
    float raycastInstance(uint32_t index, const Vec3& origin, const Vec3& dir) const;

    // Pincel de terreno (editor): cópia na CPU dos vértices do chão como estão
    // na GPU, e envio só dos vértices mudados no início do próximo frame.
    void setKeepGroundVertices(bool keep) { m_keepGroundVertices = keep; }
    bool keepGroundVertices() const { return m_keepGroundVertices; }
    std::vector<uint32_t> groundMeshes() const;
    std::vector<TerrainVertex>* groundVertices(uint32_t mesh);
    void patchGroundVertices(uint32_t mesh, const std::vector<uint32_t>& vertexIndices);
    void flushVertexPatches(VkCommandBuffer cmd);
    void growMeshBounds(uint32_t mesh, const Vec3& lo, const Vec3& hi);
    // Em lote (pincel de objetos): cópias de instâncias e liga/desliga, com um
    // só rebuild do espelho quente no fim. Devolve o índice da primeira cópia.
    uint32_t duplicateBatch(const std::vector<std::pair<uint32_t, Mat4>>& copies, const std::string& name);
    void setInstancesEnabled(const std::vector<uint32_t>& indices, bool enabled);
    void setViewHidden(const std::vector<uint32_t>& indices, bool hidden);
    // Sobe/desce várias instâncias de uma vez (um só rebuild do espelho quente).
    void offsetInstancesY(const std::vector<std::pair<uint32_t, float>>& moves);
    bool isGroundMesh(uint32_t mesh) const { return mesh < m_meshes.size() && m_meshes[mesh].isGround; }
    // Modelo já carregado (CPU), para manter o picking igual à malha editada.
    ModelAsset* assetFor(const std::string& path);
    const std::unordered_map<std::string, uint32_t>& getTextureCache() const { return m_textureCache; }

    // Publico pro popup de preview (SpherePreview.cpp) conseguir achar o
    // MESMO MRAH-W que a malha real usaria pra essa textura, em vez de sempre
    // cair no chute por luminancia (era o bug: preview chamava setMaterial
    // com pbrIndex fixo em 0).
    PbrTextureSlots resolvePbrTextures(const std::string& path);
    // Tempo gasto resolvendo texturas novas (cor e PBR) desde o ultimo zero:
    // a carga do mapa loga isso para separar textura de malha.
    struct ResolveStats { double texMs = 0.0, pbrMs = 0.0; uint32_t tex = 0, pbr = 0; };
    const ResolveStats& resolveStats() const { return m_resolveStats; }
    void resetResolveStats() { m_resolveStats = {}; }
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
    // Relevo assado que o mundo deu a esta textura: amplitude (u; 0 = nenhuma
    // malha assada le' a altura dela) e grade que o assado usou (0 = nenhuma).
    float reliefAmplitudeForSlot(uint32_t slot) const {
        auto it = m_texReliefAmp.find(slot);
        return it != m_texReliefAmp.end() ? it->second : 0.0f;
    }
    // Grade do assado nesta textura pelas malhas de um arquivo (nome do
    // arquivo; vazio = todos): a da malha com mais triangulos de origem (o
    // chao jogavel, modelado com mais detalhe que a borda), a menor e a maior. A mesma textura em outro arquivo nao entra: o
    // chao de um mapa e a montanha do vizinho dividem o slot, e a maior grade
    // de todas (a da montanha, 4-10 u) ia para o cubo da previa.
    struct ReliefGridInfo {
        float dominant = 0.0f, minSpacing = 0.0f, maxSpacing = 0.0f;
        uint32_t dominantTris = 0; // triangulos de origem da malha que da' `dominant`
        uint32_t meshes = 0;
    };
    ReliefGridInfo reliefGridForSlot(uint32_t slot, const std::string& file) const;
    // Textura do chao do mapa: 1 = cor de uma malha de chao (a que da' a altura
    // do relevo dela), 2 = so' camada do splat, 0 = nao e' do chao.
    int groundRoleForSlot(uint32_t slot) const {
        auto it = m_groundTexRole.find(slot);
        return it != m_groundTexRole.end() ? it->second : 0;
    }
    // Previa de material: a malha (espaco do modelo, UV na densidade do mundo)
    // assada como o mundo assa, com a altura da textura `texture` (slot `slot`)
    // nos vertices de pbrIndex != 0. Sem cache em disco. false = sem altura.
    bool bakeReliefPreview(std::vector<TerrainVertex>& vertices, std::vector<uint32_t>& indices, uint32_t slot,
                           const std::string& texture, float amplitude, float spacing,
                           float* usedSpacing = nullptr) const;

    // ANIMAÇÃO POR ESQUELETO (ModelSkinning.cpp). Cada instância de malha com
    // skin tem uma cópia da malha com buffer de vértices próprio; a cada frame
    // a CPU amostra a animação (matrizes dos ossos) e um compute shader escreve
    // posição e normal animadas nele. Sombra, G-buffer e o resto desenham essa
    // cópia como uma malha qualquer.
    void updateSkinning(VkCommandBuffer cmd, float dt);
    // Uma vez por frame, antes dos passes: avança o relógio dos vetores de
    // movimento (a matriz do frame anterior de cada instância).
    void beginFrame() { ++m_motionFrame; }
    // Esquece os slots de textura guardados por nome com esse prefixo (as
    // texturas foram soltas; se o nome voltar, é resolvido de novo).
    void forgetTextures(const std::string& prefix) {
        for (auto it = m_textureCache.begin(); it != m_textureCache.end();)
            it = it->first.rfind(prefix, 0) == 0 ? m_textureCache.erase(it) : std::next(it);
    }
    bool isSkinned(uint32_t instance) const { return m_skinnedByInstance.count(instance) != 0; }
    std::vector<std::string> animationsOf(uint32_t instance) const;
    // Toca a animação pelo nome (vazio = pose de repouso). Falso se não existe.
    // `fadeSeconds`: transição a partir da pose atual (0 = troca seca).
    static constexpr float kAnimationFade = 0.2f;
    bool playAnimation(uint32_t instance, const std::string& name, bool loop = true, float speed = 1.0f,
                       float fadeSeconds = kAnimationFade);
    std::string currentAnimation(uint32_t instance) const;
    // Alvos de morph da instância (nomes) e peso fixado por script (NaN solta
    // de volta para a animação). Falso se a instância não tem esse alvo.
    std::vector<std::string> morphTargetsOf(uint32_t instance) const;
    bool setMorphWeight(uint32_t instance, const std::string& target, float weight);
    size_t skinnedCount() const { return m_skinned.size(); }

    // VISUALIZADOR DE MODELO (editor): malhas de um .glb sem instância na
    // cena, os nós com malha e uma cópia animada própria para modelos com skin.
    bool loadAssetMeshes(const ModelAsset& asset); // sobe as malhas, nenhuma instância
    struct AssetNode {
        uint32_t node = 0, mesh = 0;
        Mat4 matrix{1.0f};
        std::string name;
    };
    std::vector<AssetNode> assetNodes(const std::string& path) const;
    const ModelAsset* asset(const std::string& path) const;
    const ModelMeshGPU* meshAt(uint32_t index) const { return index < m_meshes.size() ? &m_meshes[index] : nullptr; }
    bool isSkinBase(uint32_t mesh) const { return m_skinBases.count(mesh) != 0; }
    int addPreviewSkin(uint32_t baseMesh);   // id (-1: a malha não tem skin)
    uint32_t previewSkinMesh(int id);         // malha animada a desenhar
    std::vector<std::string> previewSkinAnimations(int id);
    void playPreviewSkin(int id, int anim, float speed); // anim < 0: pose de repouso
    void removePreviewSkin(int id);

private:
    struct SkinBase {
        std::shared_ptr<const SkinRig> rig;
        int skin = -1;
        uint32_t vertexCount = 0;
        VkBuffer skinBuffer = VK_NULL_HANDLE; // ossos e pesos por vértice
        VmaAllocation skinAlloc = VK_NULL_HANDLE;
        std::vector<float> jointRadius;       // alcance dos vértices de cada osso (limites)
        // Alvos de morph: deslocamentos (vértice x alvo: posição, normal), pesos
        // padrão, nomes e o nó do arquivo dos canais "weights".
        uint32_t morphTargets = 0;
        VkBuffer morphBuffer = VK_NULL_HANDLE;
        VmaAllocation morphAlloc = VK_NULL_HANDLE;
        std::vector<float> morphDefault;
        std::vector<std::string> morphNames;
        int rigNode = -1;
    };
    struct SkinnedInstance {
        uint32_t instance = 0, baseMesh = 0, mesh = 0;
        VkBuffer out = VK_NULL_HANDLE;
        VmaAllocation outAlloc = VK_NULL_HANDLE;
        VkBuffer prevOut = VK_NULL_HANDLE; // posições do frame anterior (só com o alvo de velocidade)
        VmaAllocation prevOutAlloc = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool needsInit = true;
        int anim = -1;
        float time = 0.0f, speed = 1.0f;
        bool loop = true;
        // Transição: a animação que sai continua andando e perde peso.
        int fromAnim = -1;
        float fromTime = 0.0f, fromSpeed = 1.0f;
        bool fromLoop = true;
        float fade = 0.0f, fadeSeconds = 0.0f; // fadeSeconds 0 = sem transição
        std::vector<float> morphOverride; // peso pedido pelo script (NaN = o da animação)
        uint32_t previewId = 0; // cópia do visualizador (instance == kNoInstance)
    };
    static constexpr uint32_t kNoInstance = 0xFFFFFFFFu;
    uint32_t m_previewSkinSeq = 0;
    bool createSkinned(uint32_t baseMesh, SkinnedInstance& out);
    SkinnedInstance* previewSkin(int id);
    std::unordered_map<uint32_t, SkinBase> m_skinBases; // malha base -> skin
    std::vector<SkinnedInstance> m_skinned;
    std::unordered_map<uint32_t, size_t> m_skinnedByInstance;
    VkDescriptorSetLayout m_skinSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_skinPipeLayout = VK_NULL_HANDLE;
    VkPipeline m_skinPipeline = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> m_skinPools;
    VkBuffer m_jointRing = VK_NULL_HANDLE; // matrizes dos ossos, uma faixa por frame em voo
    VmaAllocation m_jointRingAlloc = VK_NULL_HANDLE;
    glm::mat4* m_jointMapped = nullptr;
    static constexpr uint32_t kJointsPerFrame = 16384;
    bool m_skinGpuFailed = false;
    // LOD de uma malha: entrada copiada no laço de montagem, calculado em
    // paralelo (computeLod) e subido junto (applyLods).
    struct LodBuild {
        uint32_t mesh = 0;
        std::vector<struct TerrainVertex> vertices;
        std::vector<uint32_t> indices;
        bool sway = false, ground = false;
        float extent = 0.0f;
        std::string material;
        std::vector<unsigned char> loVerts, farVerts; // cartões podados: vértices próprios
        std::vector<uint32_t> lo, far, sh;
        float loSwitch = 0.0f, farSwitch = 0.0f;
        // Cadeia intermediaria (malha gigante), do mais detalhado ao menos.
        std::vector<std::vector<uint32_t>> mid;
        std::vector<std::vector<unsigned char>> midVerts; // vazio = vertex buffer da malha
        std::vector<std::vector<ModelMeshGPU::Cluster>> midClusters;
        MeshCardStats cs;     // topologia (so' folhagem): cartao x folha modelada
        bool csValid = false;
        MeshLodResult loRes, farRes, shRes; // as três partes, antes de decidir
        LodParams p;
        bool done = false; // já calculado (montagem fora da thread principal)
    };
    // Malha de um nó pronta para subir: tudo o que não depende da GPU.
    struct PreparedNode {
        bool valid = false, skinned = false, reindexed = false, isGround = false, splatGround = false;
        std::vector<struct TerrainVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<ModelMeshGPU::GroundCell> groundCells;
        Vec3 aabbMin{0.0f}, aabbMax{0.0f};
        size_t verticesBefore = 0;
        float uvPerUnit = 0.0f;
        std::vector<std::pair<uint32_t, float>> texUvPerUnit;
        bool hasLod = false;
        LodBuild lod;
    };
    static bool skinnedNode(const ModelAsset& asset, const ModelNode& node);
    static NodeSlotUse scanNodeSlotUse(const ModelAsset& asset, const ModelNode& node);
    NodeSlots resolveNodeSlots(const ModelAsset& asset, const ModelNode& node, const NodeSlotUse& use,
                               const std::vector<uint32_t>& assetTexSlots);
    static PreparedNode prepareNode(const ModelAsset& asset, size_t n, const NodeSlots& slots, const LodParams& lod,
                                    bool computeLod);
    static bool wantsLod(const LodParams& lod, bool skinned, size_t indexCount, size_t vertexCount);
    // Solda da costura do deslocamento (preset "tessellation_seam_weld").
    void createSeamWeld(ModelMeshGPU& mesh, const std::vector<struct TerrainVertex>& vertices,
                        const std::vector<uint32_t>& indices);
    // deferred: a GPU ainda pode estar lendo (descarga de asset no meio do jogo).
    void releaseSeamWeld(ModelMeshGPU& mesh, bool deferred);

public:
    struct PreparedAsset {
        std::vector<NodeSlots> slots;
        std::vector<PreparedNode> nodes;
        // compactUploadedNode feito na thread de leitura (ok = soldou); a
        // thread principal so' troca os vetores.
        struct CompactNode {
            bool ok = false;
            std::vector<ModelVertex> vertices;
            std::vector<uint32_t> indices;
        };
        std::vector<CompactNode> compact;
    };

private:
    std::unordered_map<std::string, PreparedAsset> m_prepared; // providePrepared -> próximo addModel do arquivo
    static void computeLodPart(LodBuild& b, int part);
    static void computeMidChain(LodBuild& b);
    static constexpr size_t kMidLodMinTris = 60000; // abaixo disso o salto cheia -> "lo" ja' e' curto
    static constexpr size_t kClusterMinTris = 20000; // degrau com pedacos (descarte pela borda da tela)
    static void finishLod(LodBuild& b);
    void applyLods(std::vector<LodBuild>& builds);
    void registerSkinBase(uint32_t mesh, const ModelAsset& asset, const ModelNode& node,
                          const std::vector<struct TerrainVertex>& vertices);
    void makeSkinnedInstance(uint32_t instance);
    bool ensureSkinGpu();
    void releaseSkinned(SkinnedInstance& s);
    void clearSkinning(bool destroyPipeline);

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
    std::unordered_map<uint32_t, float> m_texReliefAmp; // previa de material
    std::unordered_map<uint32_t, std::unordered_map<std::string, ReliefGridInfo>> m_texReliefGrid; // slot -> arquivo
    std::unordered_map<uint32_t, int> m_groundTexRole;
    std::unordered_map<uint32_t, PbrTextureSlots> m_pbrByTexSlot;
    std::unordered_map<std::string, std::vector<uint32_t>> m_modelMeshCache;
    // E-PISO-GRANDE (spec 12): BVH por no' (chave "arquivo#indice do no'"),
    // construida na primeira raycastInstance e reusada depois - antes era
    // triangulo por triangulo a cada chamada (custava ~1,2 ms por consulta de
    // piso numa montanha de dezenas de milhares de triangulos). Invalidada em
    // releaseAsset (prefixo "arquivo#").
    mutable std::unordered_map<std::string, RaycastBvh> m_raycastBvh;

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
    // 0 = hi, 1 = cheio, 2 = lo, 3 = far, 4.. = cadeia intermediaria (ModelMeshGPU::mid)
    static constexpr uint32_t kLodLevels = 4 + ModelMeshGPU::kMaxMidLods;
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
    // Degrau da cadeia intermediaria (1 = cheia, 4.. = mid) por triangulo/pixel.
    uint8_t midLevelFor(uint32_t idx, const ModelInstanceHot& hot, const ModelMeshGPU& mesh, float dist) const;
    std::vector<uint8_t> m_lodOfInstance;   // preenchido pelo sort, lido no draw
    Vec3 m_lastCameraPos = Vec3(0.0f);

    // ---- Instancing (SIMT) ----
    // Espelho C++ do EngInstance do model.vert (std430). NAO reordenar.
    struct GpuModelInstance {
        Mat4 model;
        Vec4 uvTranslateRot;
        Vec4 uvScaleDisp;
        Mat4 prevModel; // frame anterior: vetor de movimento de objeto que se move
    };
    // Matriz de cada instância no último frame em que foi desenhada.
    struct MotionRecord {
        Mat4 last{1.0f}, prev{1.0f};
        uint32_t frame = 0;
    };
    std::vector<MotionRecord> m_motion;
    uint32_t m_motionFrame = 1;
    const Mat4& prevTransformFor(uint32_t index, const Mat4& current);
    // Um buffer por frame-em-voo: o frame N-2 ainda LE o dele enquanto este
    // escreve (o padrao de buffer unico do SpriteRenderer tem corrida).
    static constexpr uint32_t kInstFrames = 3; // = VulkanContext::MAX_FRAMES_IN_FLIGHT
    VkDescriptorSetLayout m_instLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_instPool = VK_NULL_HANDLE;
    VkDescriptorSet m_instSets[kInstFrames] = {};
    // Set com buffer novo ainda nao escrito: e' escrito no proximo uso do
    // frame dele, quando o uso anterior ja' terminou (fence do frame).
    bool m_instSetStale[kInstFrames] = {};
    bool m_keepGroundVertices = false;
    std::unordered_map<uint32_t, std::vector<TerrainVertex>> m_groundCpu;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_vertexPatches;
    bool m_shadowSetStale[kInstFrames] = {};
    VkDescriptorSet useStorageSet(VkDescriptorSet set, bool& stale, VkBuffer buffer);
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
    uint32_t m_instPrevTotal = 0; // instancias que o quadro anterior inteiro pediu (todos os passes)
    uint32_t m_instAsked = 0;     // o mesmo, do quadro corrente (inclusive passe pulado)
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
    PopStats m_popStats;
    std::vector<uint32_t> m_popShownFrame; // ultimo quadro (m_popFrame) em que a instancia apareceu
    std::vector<uint8_t> m_popShownLod;
    std::vector<uint32_t> m_popPrev;       // visiveis no quadro anterior
    uint32_t m_popFrame = 0;
    uint32_t m_popNewFrom = 0; // indices daqui para cima nasceram neste quadro
    bool popInside(uint32_t idx) const;
    std::vector<float> m_spawnFadeStart;    // segundo (m_spawnClock) em que a entrada comecou; < 0 = nenhuma
    float m_spawnClock = 0.0f;
    std::vector<uint8_t> m_spawnNoFade;     // skipSpawnFade
    uint32_t m_spawnKnown = 0;              // instancias que ja' existiam no ultimo passe principal
    bool m_mapDrawn = false;                // o mapa ja' teve um passe principal
    bool m_spawnFadeSuspended = false;
    float spawnFadeT(uint32_t idx) const;   // 1 = inteira
    void startSpawnFade(uint32_t idx);
    std::vector<uint8_t> m_cullWhy, m_cullWhyPrev; // motivo do corte por instancia (ver PopStats)
    float m_meshLodMinDistance = 0.0f;
    // Piso da troca pro nivel "far" (cartoes 8x): o orcamento tri/px puxa o
    // far pra ~2x o lo, e com camera de diorama (700+ u) isso poe lencois em
    // meia tela. 0 = so' o piso do lo x 1,2.
    float m_meshLodFarMinDistance = 0.0f;
    float m_meshLodRatio = 0.0f;
    float m_meshLodSaved = 0.0f;
    mutable std::unordered_map<std::string, Vec4> m_footprintCache; // "arquivo#nó" -> minX, maxX, minZ, maxZ
    float m_meshLodError = 0.05f;
    float m_meshLodDistanceFactor = 8.0f;
    // Teto absoluto de triangulos do LOD. 0 = so' a fracao.
    uint32_t m_meshLodMaxTris = 0;
    uint32_t m_shadowLodMaxTris = 0;
    uint32_t m_meshLodFarMaxTris = 0;
    float m_meshLodFarDistanceMul = 3.0f;
    float m_meshLodTrisPerPx = 0.0f;   // ver setMeshLodTrisPerPx / lodSwitchFor
    uint32_t m_meshTriBudget = 0;      // ver setMeshTriBudget
    float m_lodBudgetScale = 1.0f;     // <= 1: aperta o orcamento por pixel
    float m_lodPixelScale = 1.0f;      // ver setLodPixelScale
    float trisPerPx() const { return m_meshLodTrisPerPx * m_lodBudgetScale * m_lodPixelScale; }
    static uint32_t levelTris(const ModelMeshGPU& m, uint8_t level);
    int m_shadowGroundFilter = 0;      // ver setShadowGroundFilter
    float m_shadowGroundHugeXZ = 0.0f;
    float m_screenCullMinPx = 0.0f;
    float m_shadowMinPx = 0.0f;
    // Histerese do descarte de caster por tamanho na tela: sem ela o objeto
    // que cruza o limiar liga e desliga a sombra a cada frame com a camera em
    // movimento, e o pop e' bem visivel (relatado pelo autor 2026-09-03).
    std::vector<uint8_t> m_shadowCulledPrev;
    float m_pixelsPerUnit = 0.0f;
    float m_screenW = 0.0f, m_screenH = 0.0f; // tela em px; 0 = desconhecida
    Mat4 m_lastViewProj{1.0f};
    uint32_t m_lastScreenCulled = 0;
    uint32_t m_lastShadowReceiverCulled = 0;
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
    bool m_seamWeld = false;
    // Amostrador da textura da solda: sem filtro (RGBA32F e' dado, e filtro
    // linear em formato de 32 bits nao existe em toda GPU) e sem repeticao.
    VkSampler m_weldSampler = VK_NULL_HANDLE;
    size_t m_weldBytes = 0; // VRAM das soldas vivas (teto em createSeamWeld)
    VkShaderStageFlags m_pushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    float m_tessBandDistance = 0.0f;
    float m_tessAmplitude = 0.0f;
    ReliefBakeConfig m_reliefBake;
    ReliefTextureSource m_reliefSource;
    struct ReliefFloorRef {
        // Linhas das matrizes, sem Mat4 por pergunta: o A* pergunta o chao
        // por no' expandido (Pathfinding) e o caminho do grupo dobrava o pico.
        float lx[4], lz[4], wy[4];
        float minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
        std::shared_ptr<const ReliefFloorGrid> grid;
    };
    // Gravado so' na thread principal, na carga (como m_instances); lido em
    // paralelo so' com ela parada (parallelFor da grade de andar). Baldes de
    // kReliefFloorBucket u: a grade de andar pergunta o chao de cada celula
    // do mapa, e varrer milhares de instancias por pergunta travava 200 ms.
    static constexpr float kReliefFloorBucket = 64.0f;
    std::vector<ReliefFloorRef> m_reliefFloors;
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_reliefFloorBuckets;
    void addReliefFloor(const Mat4& transform, const Vec3& wMin, const Vec3& wMax,
                        const std::shared_ptr<const ReliefFloorGrid>& grid);
    struct ReliefAlpha {
        std::string name;
        uint64_t id = 0;
        std::once_flag decoded; // so' na falta de cache, uma vez por textura
        std::vector<uint8_t> alpha;
        int w = 0, h = 0;
        bool ok = false;
    };
    struct ReliefTask {
        uint32_t mesh = 0;
        std::vector<TerrainVertex> vertices;
        std::vector<uint32_t> uploaded; // o index buffer da malha (cheia + niveis)
        uint32_t fullCount = 0;         // so' a malha cheia, no inicio de `uploaded`
        float amplitude = 0.0f;
        float spacing0 = 0.0f;       // grade pedida (0 = a do preset)
        bool store = true;           // cache em disco (a previa nao grava)
        std::vector<uint32_t> slots; // texturas que dao a altura (previa de material)
        std::vector<TerrainVertex> outVertices;
        std::vector<uint32_t> outIndices;
        std::vector<ModelMeshGPU::GroundCell> outCells;
        uint32_t outFirst = 0, outCount = 0;
        float spacing = 0.0f;
        std::shared_ptr<const ReliefFloorGrid> outFloor;
        bool ok = false, fromCache = false;
        double ms = 0.0;
    };
    using ReliefAlphas = std::unordered_map<uint32_t, std::unique_ptr<ReliefAlpha>>;
    void computeRelief(ReliefTask& task, ReliefAlphas& alphas, const std::atomic<bool>* stop = nullptr) const;
    void applyReliefs(std::vector<ReliefTask>& tasks, const std::string& label,
                      const std::unordered_map<uint32_t, std::string>& slotNames);
    // Sobe as malhas assadas (thread principal). `late`: as instancias ja'
    // existem (assado em segundo plano), entao o piso entra nelas aqui.
    void uploadReliefs(std::vector<ReliefTask>& tasks, const std::string& label, double parallelMs, bool late);
    // ASSADO EM SEGUNDO PLANO (setReliefDeferred): a malha desenha pelo
    // caminho sem assado ate' pollReliefJobs trocar. Uma tarefa por asset.
    struct ReliefJob {
        std::string label;
        std::shared_ptr<std::vector<ReliefTask>> tasks;
        std::shared_ptr<ReliefAlphas> alphas;
        std::future<void> done;
        std::chrono::steady_clock::time_point t0;
        // Ligado por dropReliefJobs: as malhas que ainda nao comecaram nem
        // entram no assado (o resultado ia para o lixo de qualquer jeito).
        std::shared_ptr<std::atomic<bool>> stop;
    };
    std::vector<ReliefJob> m_reliefJobs;
    bool m_reliefDeferred = false;
    struct StagedCopy {
        const void* data;
        VkDeviceSize size;
        VkBuffer dst;
    };
    void uploadCopies(const std::vector<StagedCopy>& ups);
    VkBuffer m_staging = VK_NULL_HANDLE;
    VmaAllocation m_stagingAlloc = VK_NULL_HANDLE;
    VkDeviceSize m_stagingSize = 0;
    // Espera e descarta as tarefas do asset (todas com nullptr): a malha vai
    // sair ou o mapa inteiro vai embora.
    void dropReliefJobs(const std::string* label);
    VkPipeline m_tessPipeline = VK_NULL_HANDLE;
    // PRE-PASSE DE PROFUNDIDADE da folhagem: m_depthPipeline so' descarta e
    // escreve profundidade; m_pipelineNoDepthWrite e' o m_pipeline sem escrita
    // de profundidade, para a mesma folhagem no G-buffer logo depois.
    VkPipeline m_depthPipeline = VK_NULL_HANDLE;
    VkPipeline m_pipelineNoDepthWrite = VK_NULL_HANDLE;
    VkPipeline m_pipelineGroundLast = VK_NULL_HANDLE; // m_pipeline com teste <, ver render()
    VkPipeline m_tessPipelineGroundLast = VK_NULL_HANDLE; // idem, o de tesselacao
    // Desenhos do passe principal, gravados para tocar duas vezes (pre-passe
    // e G-buffer). Push inteiro por desenho: a segunda passada do crossfade
    // herda campos do push anterior, e a copia preserva isso.
    struct DrawRec {
        VkBuffer vb, ib;
        uint32_t mesh, count, instances, firstIndex, firstInstance;
        uint32_t levelCount; // indices do degrau inteiro (o desenho pode ser so' um trecho)
        bool tess;
        uint8_t push[112];
    };
    std::vector<DrawRec> m_drawRecs;
    // Escala do balanco ao vento (windParams.y do quadro): folga das caixas
    // dos pedacos. Vem do passe de sombra e de setWindSwayScale.
    float m_windSwayScale = 0.15f;
public:
    void setWindSwayScale(float s) { m_windSwayScale = s; }
private:
    float m_geoDispAmplitude = 0.0f;
    float m_geoDispDistance = 0.0f;

    // Pad SoA vectors so the AVX2 kernel always sees a multiple of 8 elements.
    uint32_t m_paddedInstanceCount = 0;

    bool m_mipmapsEnabled = true;
    bool m_initialized = false;
    int m_highlightedInstanceIndex = -1;

    void createPipeline();
    void rebuildHotData();
    void captureGroundBlend(const std::vector<TerrainVertex>& vertices, size_t triangles, const Mat4& toWorld,
                            bool wantHeights);
    GroundBlendInfo m_groundBlend;
    uint32_t m_groundBlendPending = 0xFFFFFFFFu; // chao com splat novo, ainda nao capturado
    // So' as instancias acrescentadas no fim desde o ultimo espelho (addInstanceOf).
    void syncNewHotEntries();
    void syncHotEntry(uint32_t i);
    void touchInstance(uint32_t index);
    std::vector<uint32_t> m_touched;
    std::vector<uint8_t> m_touchedMark;
    uint32_t m_instanceEpoch = 0;
    void rebuildInstanceSoA();
    void appendAssetInstances(uint32_t assetIndex, const ModelInstanceDesc& ref, float centerX, float centerZ,
                              int onlyNode = -1);
    void cullInstances(const Frustum& frustum);
    static bool shadowMissesReceivers(const Frustum& receivers, const Vec3& lightTravel, float margin,
                                      const Vec3& mn, const Vec3& mx);
    void uploadMesh(const std::vector<struct TerrainVertex>& vertices,
                    const std::vector<uint32_t>& indices,
                    ModelMeshGPU& mesh);
    uint32_t resolveTexture(const std::string& path);
    ResolveStats m_resolveStats;
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
    bool m_shadowFullDetail = false;
    uint32_t m_lastDrawnTriangles = 0;
    uint32_t m_lastDrawCalls = 0;
};

} // namespace eruption
