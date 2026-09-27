#pragma once

#include <cstdlib>

#include "renderer/VulkanContext.hpp"

namespace eruption {

// MODO WIREFRAME ESTILIZADO. ERUPTION_WIREFRAME=1 liga por padrao no boot;
// F8 alterna em runtime (Engine::toggleWireframe() espera a GPU ficar
// ociosa e recria as pipelines de ModelRenderer/TerrainRenderer, que sao as
// duas unicas que consultam wireframeMode() ao construir a polygon mode).
inline bool& wireframeModeFlag() {
    static bool on = [] {
        const char* e = std::getenv("ERUPTION_WIREFRAME");
        return e && std::atoi(e) != 0;
    }();
    return on;
}
inline bool wireframeMode() { return wireframeModeFlag(); }

class GBuffer {
public:
    bool init(VulkanContext* ctx, uint32_t width, uint32_t height);
    void shutdown();
    void resize(uint32_t width, uint32_t height);

    void beginPass(VkCommandBuffer cmd);
    void endPass(VkCommandBuffer cmd);

    void transitionToRead(VkCommandBuffer cmd);
    void transitionToWrite(VkCommandBuffer cmd);

    VkImageView albedoView() const { return m_albedoView; }
    VkImageView normalView() const { return m_normalView; }
    VkImageView pbrView() const { return m_pbrView; }
    VkImageView materialView() const { return m_materialView; }
    VkImageView emissiveView() const { return m_emissiveView; }
    VkImageView depthView() const { return m_depthView; }

    VkImage albedoImage() const { return m_albedoImage; }
    VkImage normalImage() const { return m_normalImage; }
    VkImage pbrImage() const { return m_pbrImage; }
    VkImage materialImage() const { return m_materialImage; }
    VkImage emissiveImage() const { return m_emissiveImage; }
    VkImage depthImage() const { return m_depthImage; }

    VkExtent2D extent() const { return {m_width, m_height}; }

    // FONTE UNICA DOS FORMATOS DO G-BUFFER. Ate' 2026-09-03 estes valores
    // existiam mas NINGUEM os lia: GBuffer.cpp criava as imagens com literais e
    // ModelRenderer/SpriteRenderer/TerrainRenderer repetiam a lista nas suas
    // pipelines - quatro copias da mesma verdade. Duas ja' tinham divergido
    // (o .hpp dizia albedo 16F, o .cpp usava RGBA8 desde 2026-06-16), e editar
    // o .hpp nao mudava nada no binario. Agora todos leem daqui.
    static constexpr VkFormat AlbedoFormat = VK_FORMAT_R8G8B8A8_UNORM;
    // NORMAL: ainda 16F. Ver o commit que unificou os formatos - a troca para
    // 8 bits precisa ser medida de novo, agora que os literais de verdade sao
    // estes aqui.
    static constexpr VkFormat NormalFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat PbrFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat MaterialFormat = VK_FORMAT_R8_UINT;
    static constexpr VkFormat EmissiveFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat DepthFormat = VK_FORMAT_D32_SFLOAT;

    // static: os formatos sao constantes de compilacao, e as pipelines que
    // precisam deles (ModelRenderer, SpriteRenderer, TerrainRenderer) sao
    // criadas sem ter um GBuffer em maos.
    static std::vector<VkFormat> colorAttachmentFormats() {
        // WorldPos SAIU: 8 B/px de um alvo cujo conteudo ja' estava inteiro no
        // depth. Os passes de iluminacao reconstroem com worldFromDepth().
        return {AlbedoFormat, NormalFormat, PbrFormat, MaterialFormat, EmissiveFormat};
    }

private:
    VulkanContext* m_ctx = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

    // Estado de layout dos attachments (true = SHADER_READ_ONLY): evita
    // transicao COLOR->READ repetida sem write no meio (VUID 01197).
    bool m_inReadState = false;
    VkImage m_albedoImage = VK_NULL_HANDLE;
    VmaAllocation m_albedoAlloc = VK_NULL_HANDLE;
    VkImageView m_albedoView = VK_NULL_HANDLE;

    VkImage m_normalImage = VK_NULL_HANDLE;
    VmaAllocation m_normalAlloc = VK_NULL_HANDLE;
    VkImageView m_normalView = VK_NULL_HANDLE;

    VkImage m_pbrImage = VK_NULL_HANDLE;
    VmaAllocation m_pbrAlloc = VK_NULL_HANDLE;
    VkImageView m_pbrView = VK_NULL_HANDLE;

    VkImage m_materialImage = VK_NULL_HANDLE;
    VmaAllocation m_materialAlloc = VK_NULL_HANDLE;
    VkImageView m_materialView = VK_NULL_HANDLE;

    VkImage m_emissiveImage = VK_NULL_HANDLE;
    VmaAllocation m_emissiveAlloc = VK_NULL_HANDLE;
    VkImageView m_emissiveView = VK_NULL_HANDLE;


    VkImage m_depthImage = VK_NULL_HANDLE;
    VmaAllocation m_depthAlloc = VK_NULL_HANDLE;
    VkImageView m_depthView = VK_NULL_HANDLE;

    bool createAttachment(VkFormat format, VkImageUsageFlags usage,
                          VkImageAspectFlags aspect, VkImage& image,
                          VmaAllocation& alloc, VkImageView& view);
    void destroyAttachment(VkImage& image, VmaAllocation& alloc, VkImageView& view);
};

} // namespace eruption
