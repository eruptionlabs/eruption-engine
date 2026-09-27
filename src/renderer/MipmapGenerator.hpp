#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/MipMode.hpp"

namespace eruption {

class MipmapGenerator {
public:
    static uint32_t calculateMipLevels(uint32_t width, uint32_t height);

    // Inicialização do pipeline de compute
    static bool initCompute(VulkanContext* ctx);
    static void shutdownCompute(VulkanContext* ctx);

    // Gera mipmaps via vkCmdBlitImage (padrão da engine).
    // Requer que a imagem tenha sido criada com VK_IMAGE_USAGE_TRANSFER_SRC_BIT.
    // A imagem base (nível 0) deve estar em VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ao chamar.
    // Ao retornar, toda a imagem estará em VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
    static void generateMipmapsBlit(VkCommandBuffer cmd,
                                     VkImage image,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t mipLevels,
                                     VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

    // Gera mipmaps via Compute Shader (Gamma+Alpha correct)
    static void generateMipmapsCompute(VkCommandBuffer cmd,
                                       VulkanContext* ctx,
                                       VkImage image,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t mipLevels,
                                       bool isSRGB,
                                       uint32_t alphaMode);

    // Verifica se o modo atual pode ser aplicado a um asset.
    static bool canApplyMode(MipGenerationMode mode, bool hasPBR);

private:
    struct PushConstants {
        uint32_t srcWidth;
        uint32_t srcHeight;
        uint32_t numLevels;
        uint32_t gammaCorrect;
        uint32_t alphaMode;
    };

    static VkDescriptorSetLayout s_descLayout;
    static VkPipelineLayout s_pipelineLayout;
    static VkPipeline s_pipeline;
    static VkDescriptorPool s_descPool;
    static VkSampler s_sampler;
    static bool s_computeInitialized;
};

} // namespace eruption
