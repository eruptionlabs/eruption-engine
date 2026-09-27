#pragma once
// PROBES DE IRRADIANCIA (G38 / P2.4): grade 3D de visibilidade do ceu.
//
// O que o ambiente nao sabia: onde ELE NAO CHEGA. Embaixo de uma copa, dentro
// de um patio ou sob um telhado, a luz do ceu (a sonda de ceu do G36 ou o
// gradiente antigo) entrava inteira - so' o SSAO de curto alcance e o AO de
// textura escureciam, e nenhum dos dois ve a copa 30 unidades acima.
//
// Bake 2.5D no carregamento do mapa (ver probe_bake.comp): tres renders de
// profundidade com o pipeline de sombra (terreno de cima, modelos de cima,
// modelos de baixo), e por probe 64 raios marcham contra a LAJE dos modelos
// (entre o topo e a base). Sai SH L1 da visibilidade numa textura 3D RGBA16F;
// probes dentro de geometria sao dilatadas pelas vizinhas. O ambient.frag
// amostra trilinear pela posicao de mundo e multiplica o ambiente por
// E(N)/pi. Sem mapa (ou bake desligado) o lighting le' um cubo 1x1x1 "ceu
// aberto" - nunca um descritor invalido.
//
// Limites conhecidos: uma laje so' por coluna (arvore sobre casa vira uma
// laje continua); montanha nao oclui (terreno nao conta de proposito).
#include "renderer/VulkanContext.hpp"
#include "math/Types.hpp"

namespace eruption {

class TerrainRenderer;
class ModelRenderer;
class BindlessDescriptor;

class IrradianceProbes {
public:
    bool init(VulkanContext* ctx);
    void shutdown();

    // Bake SINCRONO (immediateSubmit) da cena carregada. sceneMin/Max = AABB
    // do mundo a cobrir (o chamador poe a margem vertical das copas).
    // shadowPipeline/Layout: pipeline por-draw (terreno). instPipeline/Layout:
    // pipeline INSTANCIADA dos modelos (a mesma do passe de sombra real);
    // VK_NULL_HANDLE cai no caminho por-draw.
    bool bake(TerrainRenderer& terrain, ModelRenderer& models,
              VkPipeline shadowPipeline, VkPipelineLayout shadowLayout,
              VkPipeline instPipeline, VkPipelineLayout instLayout,
              BindlessDescriptor* bindless,
              const Vec3& sceneMin, const Vec3& sceneMax);
    // Descarta a grade (mapa novo). O lighting deve voltar ao fallback.
    void clear();

    bool valid() const { return m_valid; }
    VkImageView view() const { return m_valid ? m_finalView : m_fallbackView; }
    VkSampler sampler() const { return m_sampler; }
    Vec3 gridMin() const { return m_gridMin; }
    Vec3 gridInvExtent() const { return m_gridInvExtent; }
    uint32_t dimX() const { return m_dims[0]; }
    uint32_t dimY() const { return m_dims[1]; }
    uint32_t dimZ() const { return m_dims[2]; }

    static constexpr uint32_t kDepthSize = 2048;

private:
    bool createFallback();
    bool createPipelines();
    bool createGrid(uint32_t dx, uint32_t dy, uint32_t dz);
    void destroyGrid();
    struct DepthTarget {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    bool createDepth(DepthTarget& t);
    void destroyDepth(DepthTarget& t);

    VulkanContext* m_ctx = nullptr;
    bool m_valid = false;
    Vec3 m_gridMin = Vec3(0.0f);
    Vec3 m_gridInvExtent = Vec3(0.0f);
    uint32_t m_dims[3] = {0, 0, 0};

    // Grade: bake (storage) + final (storage + sampled).
    VkImage m_bakeImage = VK_NULL_HANDLE;   VmaAllocation m_bakeAlloc = VK_NULL_HANDLE;  VkImageView m_bakeView = VK_NULL_HANDLE;
    VkImage m_finalImage = VK_NULL_HANDLE;  VmaAllocation m_finalAlloc = VK_NULL_HANDLE; VkImageView m_finalView = VK_NULL_HANDLE;
    VkImage m_fallbackImage = VK_NULL_HANDLE; VmaAllocation m_fallbackAlloc = VK_NULL_HANDLE; VkImageView m_fallbackView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;       // trilinear, clamp (grade)
    VkSampler m_depthSampler = VK_NULL_HANDLE;  // nearest (D32 nao filtra linear)

    VkBuffer m_ubo = VK_NULL_HANDLE; VmaAllocation m_uboAlloc = VK_NULL_HANDLE; void* m_uboMapped = nullptr;

    VkDescriptorSetLayout m_bakeLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_dilateLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkPipelineLayout m_bakePipeLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_dilatePipeLayout = VK_NULL_HANDLE;
    VkPipeline m_bakePipeline = VK_NULL_HANDLE;
    VkPipeline m_dilatePipeline = VK_NULL_HANDLE;
};

} // namespace eruption
