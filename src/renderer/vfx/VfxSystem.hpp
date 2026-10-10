#pragma once

// SISTEMA DE PARTICULAS (VFX) NA GPU.
//
// Todas as particulas de todos os efeitos moram num pool unico na GPU:
//   - nascimento: a CPU so' CONTA quantas nascem de cada emissor no frame
//     (taxa, rajada, distancia) e manda a lista de pedidos; a GPU tira os
//     indices de uma lista de mortas e inicializa (vfx_emit);
//   - simulacao: um compute sobre a lista de vivas, com dispatch indireto
//     (vfx_args + vfx_sim), que monta a lista do proximo frame e a de desenho;
//   - desenho: UM draw indireto para tudo (vfx_finish escreve os argumentos).
// Nada volta da GPU para a CPU durante o jogo (sem leitura de contagem): as
// estatisticas do editor/telemetria leem os contadores com frames de atraso.
//
// Cada instancia de efeito ocupa um "slot" de emissor por emissor do efeito;
// o slot tem geracao, entao particula de efeito que ja' acabou morre sozinha
// quando o slot e' reaproveitado.

#include "math/Types.hpp"
#include "renderer/VulkanContext.hpp"
#include "renderer/vfx/VfxEffect.hpp"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace eruption {

class BindlessDescriptor;
class Camera;

// Espelho em C++ de `Emitter` (shaders/vfx/vfx_common.glsl), std430.
struct VfxEmitterGpu {
    Vec4 posTime;
    Vec4 prevPos;
    Vec4 rot0, rot1, rot2;
    Vec4 velInherit;
    Vec4 shape;
    glm::uvec4 flags;
    Vec4 life;
    Vec4 dirSpread;
    Vec4 sizeRot;
    Vec4 angVel;
    Vec4 gravityDrag;
    Vec4 noise;
    Vec4 collide;
    Vec4 render;
    Vec4 flip;
    uint32_t colorLut[16];
    uint32_t sizeLut[8];
    glm::uvec4 cutout; // 8 halfs: poligono justo da textura
    glm::uvec4 graphA; // grafo: velocidade inicial (inicio, tamanho), forca
    glm::uvec4 graphB; // grafo: cor, tamanho
};
static_assert(sizeof(VfxEmitterGpu) == 416, "VfxEmitterGpu precisa bater com o std430 do shader");

// Textura de uma folha de quadros pronta para o desenho.
struct VfxTextureInfo {
    uint32_t slot = 0xFFFFFFFFu; // 0xFFFFFFFF = disco procedural
    float cutout[8] = {1, 1, 1, 1, 1, 1, 1, 1};
};

struct VfxSpawnRequestGpu {
    uint32_t slot, count, first, seed;
};

// Espelho de `FrameParams` (std140).
struct VfxFrameParamsGpu {
    Mat4 viewProj;
    Mat4 invViewProj;
    Vec4 frustum[6];
    Vec4 camPos;
    Vec4 timeInfo;
    Vec4 depthInfo;
    glm::uvec4 counts;
};

class VfxSystem {
public:
    struct InitInfo {
        VulkanContext* ctx = nullptr;
        BindlessDescriptor* bindless = nullptr;
        VkDescriptorSetLayout frameUboLayout = VK_NULL_HANDLE;
        VkDescriptorSet frameUboSet = VK_NULL_HANDLE;
        uint32_t capacity = 131072;   // particulas vivas no total (pool)
        // PNG (ou builtin:) -> slot bindless + poligono justo para a grade de
        // quadros dada (o motor sobe a textura uma vez por arquivo).
        std::function<VfxTextureInfo(const std::string&, uint32_t cols, uint32_t rows)> resolveTexture;
        // Nome do efeito -> arquivo .vfx.json (projeto primeiro, motor depois).
        std::function<std::string(const std::string&)> resolveEffectPath;
    };

    struct Stats {
        uint32_t alive = 0;      // lido da GPU com atraso (so' estatistica)
        uint32_t rendered = 0;
        uint32_t instances = 0;
        uint32_t emitters = 0;
        uint32_t spawnedThisFrame = 0;
        uint32_t capacity = 0;
    };

    bool init(const InitInfo& info);
    void shutdown();

    // Efeitos sao descritos em METROS; o mundo pode ter outra unidade
    // (unidades por metro). Vale para todas as instancias novas.
    void setUnitsPerMeter(float u) { m_unitsPerMeter = std::max(u, 1e-4f); }
    float unitsPerMeter() const { return m_unitsPerMeter; }

    // Orcamento do preset grafico: pool total e escala de nascimento.
    void setBudget(uint32_t capacity, float spawnScale);
    uint32_t capacity() const { return m_capacity; }

    // --- Efeitos (assets) ---
    // Carrega sob demanda e guarda. nullptr = nao achou/arquivo invalido.
    const VfxEffectDesc* effect(const std::string& name);
    // Troca a descricao em memoria (editor ao vivo): instancias tocando
    // passam a usar a nova a partir do proximo frame.
    void setEffect(const std::string& name, const VfxEffectDesc& desc);
    void forgetEffect(const std::string& name);

    // --- Instancias ---
    uint32_t spawn(const std::string& effectName, const Mat4& transform); // 0 = falhou
    void setTransform(uint32_t id, const Mat4& transform);
    void stop(uint32_t id, bool immediate = false); // sem immediate: para de nascer, as vivas terminam
    void restart(uint32_t id);
    bool isPlaying(uint32_t id) const;
    float instanceTime(uint32_t id) const;
    void clear(); // mata tudo (troca de mapa)

    // --- Frame ---
    // CPU: tempo, pedidos de nascimento e parametros. Chamar antes de simulate().
    void update(float dt, const Camera& camera, VkExtent2D renderSize);
    // Compute (depois do G-buffer: a colisao usa a profundidade deste frame,
    // que precisa estar em SHADER_READ_ONLY_OPTIMAL).
    void simulate(VkCommandBuffer cmd, VkImageView depthView);
    // Camada de meia resolucao (fumaca): FORA de qualquer rendering, antes de
    // draw(). Sem emissor lowRes ativo nao faz nada.
    void drawLowRes(VkCommandBuffer cmd, VkImageView depthView, VkExtent2D extent);
    // Rendering proprio sobre `litView` (RGBA16F em COLOR_ATTACHMENT) com a
    // profundidade so' leitura: compoe a meia resolucao, desenha as particulas
    // e escreve a cobertura delas na mascara reativa (R8, para o FSR).
    void draw(VkCommandBuffer cmd, VkImageView litView, VkImageView depthView, VkExtent2D extent);
    // Fim do frame: sem particula, limpa a mascara uma vez (nao fica rastro).
    void endFrame(VkCommandBuffer cmd);
    // Tamanho de render (cria a mascara reativa). Chamar sem frame em voo
    // usando a imagem antiga (o motor chama no bloco de resize).
    bool resize(uint32_t w, uint32_t h);
    VkImageView reactiveView() const { return m_reactiveView; }
    // Algo para simular/desenhar? (sem nenhum efeito, o sistema custa zero)
    bool active() const { return m_active; }
    // Troca o FrameUBO do desenho (camera propria por frame, ex.: previa).
    void setFrameUboSet(VkDescriptorSet set) { m_frameUboSet = set; }

    const Stats& stats() const { return m_stats; }
    // Depuracao: le' (com espera) as primeiras particulas da lista de desenho.
    void debugDump();

private:
    struct EmitterState {
        uint32_t slot = 0;
        float spawnAccum = 0.0f;
        float burstTimer = 0.0f;
        uint32_t burstsDone = 0;
        // Estimativa de vivas sem leitura da GPU: (morte prevista, quantidade).
        std::vector<std::pair<float, uint32_t>> liveChunks;
        uint32_t liveEstimate = 0;
    };
    struct Instance {
        uint32_t id = 0;
        std::string effect;
        std::shared_ptr<const VfxEffectDesc> desc;
        Mat4 transform{1.0f};
        Vec3 prevPos{0.0f};
        Vec3 velocity{0.0f};
        float time = 0.0f;
        float stopTime = -1.0f; // >= 0: parou de nascer neste tempo
        bool killed = false;
        bool firstFrame = true;
        std::vector<EmitterState> emitters;
    };

    bool createPipelines();
    bool createBuffers();
    void destroyBuffers();
    bool createDescriptors();
    void writeDescriptors(uint32_t frame, VkImageView depthView);
    uint32_t allocSlot();
    void freeSlot(uint32_t slot);
    void fillEmitterGpu(const Instance& inst, const VfxEmitterDesc& e, uint32_t slot, VfxEmitterGpu& out);
    const VfxTextureInfo& texture(const std::string& path, uint32_t cols, uint32_t rows);
    bool ensureLowResTarget(VkExtent2D full);

    VulkanContext* m_ctx = nullptr;
    BindlessDescriptor* m_bindless = nullptr;
    VkDescriptorSetLayout m_frameUboLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_frameUboSet = VK_NULL_HANDLE;
    std::function<VfxTextureInfo(const std::string&, uint32_t, uint32_t)> m_resolveTexture;
    std::function<std::string(const std::string&)> m_resolveEffectPath;
    std::unordered_map<std::string, VfxTextureInfo> m_textures; // "caminho#colsxrows" 

    // Pipelines
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_computeLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_drawLayout = VK_NULL_HANDLE;
    VkPipeline m_emitPipe = VK_NULL_HANDLE;
    VkPipeline m_argsPipe = VK_NULL_HANDLE;
    VkPipeline m_simPipe = VK_NULL_HANDLE;
    VkPipeline m_finishPipe = VK_NULL_HANDLE;
    VkPipeline m_drawPipe = VK_NULL_HANDLE;
    VkPipeline m_drawLowPipe = VK_NULL_HANDLE;   // meia resolucao, sem attachment de profundidade
    VkPipeline m_compositePipe = VK_NULL_HANDLE; // meia resolucao -> lit
    VkPipeline m_sortHistPipe = VK_NULL_HANDLE, m_sortScanPipe = VK_NULL_HANDLE, m_sortScatterPipe = VK_NULL_HANDLE;
    VkPipelineLayout m_compositeLayout = VK_NULL_HANDLE;
    VkSampler m_linearSampler = VK_NULL_HANDLE;
    // Alvo de meia resolucao (RGBA16F, cor premultiplicada + cobertura).
    VkImage m_lowImage = VK_NULL_HANDLE;
    VmaAllocation m_lowAlloc = VK_NULL_HANDLE;
    VkImageView m_lowView = VK_NULL_HANDLE;
    VkExtent2D m_lowExtent{0, 0};
    VkExtent2D m_renderSize{0, 0};
    // Mascara reativa do FSR (cobertura das particulas), na resolucao de render.
    VkImage m_reactiveImage = VK_NULL_HANDLE;
    VmaAllocation m_reactiveAlloc = VK_NULL_HANDLE;
    VkImageView m_reactiveView = VK_NULL_HANDLE;
    VkExtent2D m_reactiveExtent{0, 0};
    bool m_reactiveWritten = false;
    void destroyReactive();
    std::array<VkImageView, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_setLow{};
    VkSampler m_depthSampler = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    // Dois sets por frame: a profundidade esta' em SHADER_READ_ONLY na
    // simulacao e em DEPTH_STENCIL_READ_ONLY no desenho (attachment ao mesmo tempo).
    std::array<VkDescriptorSet, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_sets{};
    std::array<VkDescriptorSet, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_drawSets{};
    std::array<VkImageView, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_setDepth{};
    std::array<bool, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_setBuffersWritten{};

    // Buffers na GPU
    struct Buf {
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };
    Buf m_particles, m_dead, m_alive, m_counters, m_render, m_sort;
    // Por frame em voo: emissores, pedidos, parametros, leitura dos contadores.
    static constexpr uint32_t kMaxSlots = 4096;
    static constexpr uint32_t kMaxRequests = 4096;
    std::array<Buf, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_emitterBuf, m_spawnBuf, m_frameBuf, m_readback;
    std::array<bool, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_readbackPending{};
    std::array<uint32_t, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_readbackList{};
    Buf m_emitterDev, m_spawnDev, m_frameDev; // copias na memoria da placa

    // Grafos: programas compilados de todos os emissores com grafo, num
    // codigo e num pool de constantes unicos (enderecos absolutos).
    static constexpr uint32_t kGraphCodeWords = 65536;
    static constexpr uint32_t kGraphConsts = 16384;
    Buf m_graphCode, m_graphConst;
    std::array<Buf, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_graphStage;
    std::unordered_map<std::string, VfxGraphCompiled> m_graphLib; // chave: texto do grafo
    std::vector<uint32_t> m_graphCodeCpu;
    std::vector<Vec4> m_graphConstCpu;
    bool m_graphDirty = false;
    const VfxGraphCompiled& graphFor(const VfxGraph& g);
    bool createBuf(Buf& b, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage mem, bool map);
    void destroyBuf(Buf& b);

    bool m_ready = false;
    uint32_t m_capacity = 0;
    float m_spawnScale = 1.0f;
    float m_time = 0.0f;
    float m_unitsPerMeter = 1.0f;
    float m_maxLifeSeen = 0.0f;
    uint32_t m_cur = 0;          // lista de vivas atual (0/1)
    bool m_resetPool = true;     // reinicia a lista de mortas no proximo simulate
    bool m_active = false;
    bool m_anyLowRes = false;   // algum emissor lowRes tocando (CPU sabe)
    bool m_sortThisFrame = false; // algum emissor com mistura alfa tocando
    float m_idleTime = 1e9f;     // tempo sem instancia nenhuma

    // Estado do frame (montado em update, consumido em simulate)
    std::vector<VfxSpawnRequestGpu> m_requests;
    uint32_t m_spawnTotal = 0;
    uint32_t m_slotHighWater = 0;
    VfxFrameParamsGpu m_frameParams{};
    uint32_t m_frameSeed = 1;

    std::vector<VfxEmitterGpu> m_slotData;     // por slot (CPU, sobe ate' highWater)
    std::vector<uint16_t> m_slotGen;
    std::vector<uint32_t> m_freeSlots;

    std::unordered_map<std::string, std::shared_ptr<VfxEffectDesc>> m_effects;
    std::vector<Instance> m_instances;
    uint32_t m_nextId = 1;
    Stats m_stats;
};

} // namespace eruption
