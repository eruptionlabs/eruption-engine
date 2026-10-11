#include "renderer/PushRing.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <cstring>

namespace eruption {

bool PushRing::init(VulkanContext* ctx, VkShaderStageFlags stages, uint32_t maxBlock, uint32_t bytesPerFrame) {
    m_ctx = ctx;
    m_maxBlock = maxBlock;
    m_bytesPerFrame = bytesPerFrame;
    m_align = static_cast<uint32_t>(std::max<VkDeviceSize>(
        16, ctx->deviceProperties().limits.minUniformBufferOffsetAlignment));

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b.descriptorCount = 1;
    b.stageFlags = stages;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(ctx->device(), &li, nullptr, &m_layout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VulkanContext::MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = VulkanContext::MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(ctx->device(), &pi, nullptr, &m_pool) != VK_SUCCESS) return false;

    for (Slot& s : m_slots) {
        if (!ctx->createBuffer(bytesPerFrame, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
                               s.buffer, s.alloc))
            return false;
        void* p = nullptr;
        vmaMapMemory(ctx->allocator(), s.alloc, &p);
        s.mapped = static_cast<uint8_t*>(p);

        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_layout;
        if (vkAllocateDescriptorSets(ctx->device(), &ai, &s.set) != VK_SUCCESS) return false;

        VkDescriptorBufferInfo bi{s.buffer, 0, maxBlock};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = s.set;
        w.dstBinding = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w.descriptorCount = 1;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(ctx->device(), 1, &w, 0, nullptr);
    }
    return true;
}

void PushRing::shutdown() {
    if (!m_ctx) return;
    for (Slot& s : m_slots) {
        if (s.mapped) vmaUnmapMemory(m_ctx->allocator(), s.alloc);
        if (s.buffer) vmaDestroyBuffer(m_ctx->allocator(), s.buffer, s.alloc);
        s = Slot{};
    }
    if (m_pool) vkDestroyDescriptorPool(m_ctx->device(), m_pool, nullptr);
    if (m_layout) vkDestroyDescriptorSetLayout(m_ctx->device(), m_layout, nullptr);
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_ctx = nullptr;
}

void PushRing::push(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipelineLayout pipelineLayout,
                    uint32_t setIndex, const void* data, uint32_t size) {
    // O slot do quadro em voo so' volta a ser escrito depois que a GPU acabou
    // com ele (a cerca do quadro), entao trocar de slot zera o cursor.
    const uint32_t frame = m_ctx->currentFrame() % VulkanContext::MAX_FRAMES_IN_FLIGHT;
    if (frame != m_cursorFrame) {
        m_cursorFrame = frame;
        m_cursor = 0;
    }
    if (m_cursor + m_maxBlock > m_bytesPerFrame) {
        if (!m_warned) {
            m_warned = true;
            ERUPTION_LOG_WARN("[PushRing] anel cheio (%u bytes por quadro): reaproveitando o ultimo bloco",
                              m_bytesPerFrame);
        }
        m_cursor = m_bytesPerFrame - ((m_maxBlock + m_align - 1) / m_align) * m_align;
    }
    Slot& s = m_slots[frame];
    std::memcpy(s.mapped + m_cursor, data, std::min(size, m_maxBlock));
    const uint32_t offset = m_cursor;
    m_cursor += ((m_maxBlock + m_align - 1) / m_align) * m_align;
    vkCmdBindDescriptorSets(cmd, bindPoint, pipelineLayout, setIndex, 1, &s.set, 1, &offset);
}

} // namespace eruption
