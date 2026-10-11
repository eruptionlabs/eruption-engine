#pragma once

#include "renderer/VulkanContext.hpp"

#include <array>
#include <cstdint>

namespace eruption {

// Dados por draw maiores que o limite de push constants da GPU.
//
// O Vulkan so' garante 128 bytes de push constant; GPU de celular (Xclipse,
// Mali) para ai'. Passar disso corrompe a memoria do driver (crash no S24) e
// o shader le' lixo no fim do bloco. O PushRing faz o mesmo papel com um UBO
// de offset dinamico: cada push() copia o bloco para um anel por quadro em
// voo e liga o set com o offset daquele draw. No shader o bloco so' troca a
// declaracao: `layout(push_constant)` vira `layout(std140, set = N, binding = 0)`
// (mesmo layout para blocos de mat4/vec4/escalares; arrays mudam em std140).
class PushRing {
public:
    // maxBlock: maior bloco que um draw envia. bytesPerFrame: teto por quadro.
    bool init(VulkanContext* ctx, VkShaderStageFlags stages, uint32_t maxBlock, uint32_t bytesPerFrame);
    void shutdown();

    VkDescriptorSetLayout layout() const { return m_layout; }

    // Copia `size` bytes e liga o set `setIndex` de `pipelineLayout` no offset.
    void push(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout pipelineLayout,
              uint32_t setIndex, const void* data, uint32_t size);

private:
    VulkanContext* m_ctx = nullptr;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        uint8_t* mapped = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    std::array<Slot, VulkanContext::MAX_FRAMES_IN_FLIGHT> m_slots{};
    uint32_t m_bytesPerFrame = 0;
    uint32_t m_maxBlock = 0;
    uint32_t m_align = 256;
    uint32_t m_cursor = 0;
    uint32_t m_cursorFrame = UINT32_MAX;
    bool m_warned = false;
};

} // namespace eruption
