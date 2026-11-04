#if defined(__EMSCRIPTEN__)
#include "platform/web/WebMirror.hpp"
#endif
#include "renderer/BindlessDescriptor.hpp"
#include "core/Logger.hpp"
#include "renderer/ShaderCompiler.hpp"
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace eruption {

// Quantos slots o array sem tamanho fixo pode ter nesta GPU. 4096 enchia com
// quatro folhas de personagem (um slot por quadro de sprite, ~3550) mais um
// mapa vizinho. O limite da GPU vale para o estagio inteiro, entao sobra
// folga para as outras imagens dos pipelines (G-buffer, sombra, pos).
static uint32_t indexedSlotCount(VulkanContext* ctx) {
    VkPhysicalDeviceDescriptorIndexingProperties idx{};
    idx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &idx;
    vkGetPhysicalDeviceProperties2(ctx->physicalDevice(), &p2);
    constexpr uint32_t kFolga = 64;
    const uint32_t lim = std::min(idx.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                  idx.maxDescriptorSetUpdateAfterBindSampledImages);
    const uint32_t cabe = lim > kFolga ? lim - kFolga : 0;
    return std::clamp(cabe, MAX_BINDLESS_TEXTURES, MAX_BINDLESS_TEXTURES_INDEXED);
}

bool BindlessDescriptor::init(VulkanContext* ctx) {
    m_ctx = ctx;
    m_layered = !ctx->bindless();
    m_slotCount = m_layered ? MAX_BINDLESS_TEXTURES : indexedSlotCount(ctx);
    ShaderCompiler::setNoBindless(m_layered);
    if (!createLayout()) return false;
    if (!createPool()) return false;
    if (!allocateSet()) return false;

    // Create a default 2x2 neutral gray for slot 0 (avoids magenta artifacts)
    uint8_t checkerboard[] = {
        128, 128, 128, 255,   128, 128, 128, 255,
        128, 128, 128, 255,   128, 128, 128, 255
    };
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {2, 2, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    vmaCreateImage(m_ctx->allocator(), &imageInfo, &allocInfo, &m_defaultImage, &m_defaultAlloc, nullptr);

    VkBuffer staging;
    VmaAllocation stagingAlloc;
    m_ctx->createBuffer(16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
    void* mapped;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, checkerboard, 16);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        m_ctx->cmdImageBarrier(cmd, m_defaultImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            0, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {2, 2, 1};
        vkCmdCopyBufferToImage(cmd, staging, m_defaultImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        m_ctx->cmdImageBarrier(cmd, m_defaultImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    });

    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_defaultImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc::createImageView(m_ctx->device(), &viewInfo, nullptr, &m_defaultView);

    VkSamplerCreateInfo samplerInfo{}; samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_defaultSampler);

    if (m_layered) {
        // Lado da camada: ERUPTION_TEX_LAYER_SIZE (potência de 2) vence a
        // chave "texture_layer_size" do preset (Engine::init), e o padrao e'
        // 256 - cabe na memória de um celular de 4 GB.
        uint32_t layerSize = m_layerSizeRequest ? m_layerSizeRequest : 256;
        if (const char* e = std::getenv("ERUPTION_TEX_LAYER_SIZE")) layerSize = static_cast<uint32_t>(std::atoi(e));
        // Camadas reservadas no inicio: ERUPTION_TEX_LAYER_RESERVE vence a chave
        // "texture_layer_reserve" do preset (Engine::init); 0 = so' o minimo.
        uint32_t reserve = m_layerReserveRequest;
        if (const char* e = std::getenv("ERUPTION_TEX_LAYER_RESERVE")) reserve = static_cast<uint32_t>(std::atoi(e));
        // Idem para o segundo array: ERUPTION_TEX_LAYER_RESERVE_B / "texture_layer_reserve_b".
        uint32_t reserveB = m_layerReserveBRequest;
        if (const char* e = std::getenv("ERUPTION_TEX_LAYER_RESERVE_B")) reserveB = static_cast<uint32_t>(std::atoi(e));
        if (!m_layers.init(m_ctx, m_slotCount, layerSize, m_frameSets, m_defaultView, m_defaultSampler, reserve,
                           reserveB))
            return false;
        m_ctx->addFrameBeginHook([this](uint32_t frame) { onFrameBegin(frame); });
    } else {
        updateTexture(0, m_defaultView, m_defaultSampler);
        flushUpdates();
    }

    // Pre-allocate free list backwards for better cache locality
    m_freeSlots.reserve(m_slotCount);
    for (uint32_t i = m_slotCount - 1; i >= 1; --i) {
        m_freeSlots.push_back(i);
    }

    ERUPTION_LOG_WARN("Bindless descriptor system initialized (%u slots, default at slot 0)", m_slotCount);
    return true;
}

void BindlessDescriptor::shutdown() {
    if (m_defaultView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_ctx->device(), m_defaultView, nullptr);
        m_defaultView = VK_NULL_HANDLE;
    }
    if (m_defaultSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device(), m_defaultSampler, nullptr);
        m_defaultSampler = VK_NULL_HANDLE;
    }
    if (m_defaultImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_ctx->allocator(), m_defaultImage, m_defaultAlloc);
        m_defaultImage = VK_NULL_HANDLE;
        m_defaultAlloc = VK_NULL_HANDLE;
    }

    m_layers.shutdown();
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_layout, nullptr);
        m_layout = VK_NULL_HANDLE;
    }
    m_set = VK_NULL_HANDLE;
    for (auto& set : m_frameSets) set = VK_NULL_HANDLE;
    m_pendingUpdates.clear();
    m_freeSlots.clear();
}

uint32_t BindlessDescriptor::allocateSlot() {
    if (!m_freeSlots.empty()) {
        uint32_t slot = m_freeSlots.back();
        m_freeSlots.pop_back();
        ++m_allocated; m_highWater = std::max(m_highWater, m_allocated);
        return slot;
    }
    // A lista de livres nasce com todos os slots (init), entao vazia = cheio.
    // Aqui havia um m_nextSlot++ que devolvia 1, 2, 3... ainda em uso: a
    // textura nova tomava o lugar de outra sem erro nenhum (chao e modelos
    // amostrando quadros de sprite, medido com 4096 slots esgotados por
    // 3552 quadros de personagem + as texturas de um vizinho).
    static uint32_t s_fullCount = 0;
    if (s_fullCount++ % 256 == 0)
        ERUPTION_LOG_ERROR("Bindless texture array full! (max=%u, %u requests refused)", m_slotCount, s_fullCount);
    return 0; // slot 0 = default/null
}

uint32_t BindlessDescriptor::allocateRange(uint32_t count) {
    // A lista de livres comeca com TODOS os slots (init) e allocateSlot so'
    // tira dela: a sequencia tem de sair da propria lista, o menor inicio que
    // tenha `count` slots livres seguidos.
    if (count == 0) return 0;
    std::vector<bool> livre(m_slotCount, false);
    for (uint32_t s : m_freeSlots) livre[s] = true;
    uint32_t first = 0;
    for (uint32_t s = 1, run = 0; s < m_slotCount; ++s) {
        run = livre[s] ? run + 1 : 0;
        if (run == count) { first = s + 1 - count; break; }
    }
    if (first == 0) return 0;
    m_freeSlots.erase(std::remove_if(m_freeSlots.begin(), m_freeSlots.end(),
                                     [&](uint32_t s) { return s >= first && s < first + count; }),
                      m_freeSlots.end());
    m_allocated += count;
    m_highWater = std::max(m_highWater, m_allocated);
    return first;
}

void BindlessDescriptor::freeSlot(uint32_t slot) {
    if (slot == 0 || slot >= m_slotCount) return;
    if (m_allocated > 0) --m_allocated;
    m_freeSlots.push_back(slot);
    // Reset slot to default texture so shader doesn't access destroyed image
    if (m_layered) m_pendingUpdates.push_back({slot, VK_NULL_HANDLE, VK_NULL_HANDLE});
    else m_pendingUpdates.push_back({slot, m_defaultView, m_defaultSampler});
}

void BindlessDescriptor::updateTexture(uint32_t slot, VkImageView view, VkSampler sampler, bool dynamic) {
    if (slot >= m_slotCount) return;
    // Slot 0 e' a textura padrao, devolvida quando o array enche: quem nao
    // confere o slot nao pode trocar a padrao de todo mundo.
    if (slot == 0 && view != m_defaultView) return;
#if defined(__EMSCRIPTEN__)
    web::setSlotView(slot, view); // o desenho WebGL2 le a textura pelo slot
#endif
    m_pendingUpdates.push_back({slot, view, sampler, dynamic});
}

void BindlessDescriptor::markNearestSampler(VkSampler sampler) {
    if (!m_layered) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_layers.markNearest(sampler);
}

void BindlessDescriptor::flushUpdates() {
    if (m_pendingUpdates.empty()) return;
    if (m_layered) {
        // As cópias e os sets ficam para o próximo início de frame (ou syncNow).
        for (const auto& u : m_pendingUpdates) {
            if (u.view == VK_NULL_HANDLE) m_layers.release(u.slot);
            else m_layers.assign(u.slot, u.view, u.sampler, u.dynamic);
        }
        m_pendingUpdates.clear();
        return;
    }
    writeSet(m_set, m_pendingUpdates);
    m_pendingUpdates.clear();
}

void BindlessDescriptor::syncNow() {
    if (!m_layered) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    flushUpdates();
    m_layers.applyAll();
}

void BindlessDescriptor::onFrameBegin(uint32_t frame) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_layers.apply(frame);
    }
    if (m_afterCopies) m_afterCopies();
}

void BindlessDescriptor::writeSet(VkDescriptorSet set, const std::vector<PendingUpdate>& updates) {
    if (updates.empty()) return;

    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(updates.size());
    std::vector<VkDescriptorImageInfo> imageInfos;
    imageInfos.reserve(updates.size());

    for (const auto& upd : updates) {
        imageInfos.push_back({upd.sampler, upd.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }

    for (size_t i = 0; i < updates.size(); ++i) {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.dstArrayElement = updates[i].slot;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfos[i];
        writes.push_back(write);
    }

    vkUpdateDescriptorSets(m_ctx->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

uint32_t BindlessDescriptor::allocateSlotSafe() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return allocateSlot();
}

void BindlessDescriptor::freeSlotSafe(uint32_t slot) {
    std::lock_guard<std::mutex> lock(m_mutex);
    freeSlot(slot);
}

void BindlessDescriptor::updateTextureSafe(uint32_t slot, VkImageView view, VkSampler sampler, bool dynamic) {
    std::lock_guard<std::mutex> lock(m_mutex);
    updateTexture(slot, view, sampler, dynamic);
}

void BindlessDescriptor::flushUpdatesSafe() {
    std::lock_guard<std::mutex> lock(m_mutex);
    flushUpdates();
}

bool BindlessDescriptor::createLayout() {
    if (m_layered) {
        const auto bindings = TextureLayers::layoutBindings();
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        VK_CHECK(vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_layout));
        return true;
    }
    VkDescriptorSetLayoutBinding textureBinding{};
    textureBinding.binding = 0;
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureBinding.descriptorCount = m_slotCount;
    textureBinding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    textureBinding.pImmutableSamplers = nullptr;

    VkDescriptorBindingFlags bindingFlags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = 1;
    flagsInfo.pBindingFlags = &bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &textureBinding;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.pNext = &flagsInfo;

    VK_CHECK(vkCreateDescriptorSetLayout(m_ctx->device(), &layoutInfo, nullptr, &m_layout));
    return true;
}

bool BindlessDescriptor::createPool() {
    const uint32_t sets = m_layered ? VulkanContext::MAX_FRAMES_IN_FLIGHT : 1u;
    std::vector<VkDescriptorPoolSize> poolSizes;
    if (m_layered) poolSizes = TextureLayers::poolSizes(sets);
    else poolSizes.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_slotCount});

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    if (!m_layered) poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = sets;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    VK_CHECK(vkCreateDescriptorPool(m_ctx->device(), &poolInfo, nullptr, &m_pool));
    return true;
}

bool BindlessDescriptor::allocateSet() {
    if (m_layered) {
        VkDescriptorSetLayout layouts[VulkanContext::MAX_FRAMES_IN_FLIGHT];
        for (auto& l : layouts) l = m_layout;
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_pool;
        allocInfo.descriptorSetCount = VulkanContext::MAX_FRAMES_IN_FLIGHT;
        allocInfo.pSetLayouts = layouts;
        VK_CHECK(vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, m_frameSets));
        m_set = m_frameSets[0];
        return true;
    }
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{};
    variableInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    uint32_t maxCount = m_slotCount;
    variableInfo.descriptorSetCount = 1;
    variableInfo.pDescriptorCounts = &maxCount;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_layout;
    allocInfo.pNext = &variableInfo;

    VK_CHECK(vkAllocateDescriptorSets(m_ctx->device(), &allocInfo, &m_set));
    return true;
}

} // namespace eruption
