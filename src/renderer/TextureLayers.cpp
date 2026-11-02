#if defined(__EMSCRIPTEN__)
#include "platform/web/WebMirror.hpp"
#endif
#include "renderer/TextureLayers.hpp"

#include "core/Logger.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/VkCompat.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace eruption {

namespace {

// Formatos que a cópia leva para RGBA8 sem perder nada: 8 bits por canal e os
// comprimidos de faixa normal. Float e profundidade vão para ligação direta.
bool copyable(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB:
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        return false;
    default:
        // BC1..BC7, ETC2/EAC e ASTC LDR são faixas contíguas no enum.
        return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) ||
               (f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK) ||
               (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
    }
}

bool isSrgb(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        return true;
    default:
        // ASTC: cada tamanho de bloco vem em par UNORM/SRGB, o SRGB depois.
        return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK &&
               (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) % 2 == 1;
    }
}

VkImageMemoryBarrier layerBarrier(VkImage image, uint32_t layer, uint32_t baseMip, uint32_t mipCount,
                                  VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, layer, 1};
    return b;
}

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst, const VkImageMemoryBarrier& b) {
    vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkShaderModule loadModule(VkDevice device, const char* path) {
    const auto code = ShaderCompiler::loadSPIRV(path);
    if (code.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size() * sizeof(uint32_t);
    ci.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &module);
    return module;
}

} // namespace

std::vector<VkDescriptorSetLayoutBinding> TextureLayers::layoutBindings() {
    std::vector<VkDescriptorSetLayoutBinding> bindings(kBindingCount);
    for (uint32_t i = 0; i < kBindingCount; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = i == kTableBinding ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                        : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return bindings;
}

std::vector<VkDescriptorPoolSize> TextureLayers::poolSizes(uint32_t sets) {
    return {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (kBindingCount - 1) * sets},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets}};
}

bool TextureLayers::init(VulkanContext* ctx, uint32_t slotCount, uint32_t layerSize, const VkDescriptorSet* sets,
                         VkImageView defaultView, VkSampler defaultSampler) {
    m_ctx = ctx;
    m_layerSize = 1;
    while (m_layerSize * 2 <= std::clamp(layerSize, 16u, 4096u)) m_layerSize *= 2;
    m_mipLevels = 1;
    while ((m_layerSize >> m_mipLevels) > 0) ++m_mipLevels;
    m_defaultView = defaultView;
    m_defaultSampler = defaultSampler;
    for (uint32_t f = 0; f < kFrames; ++f) m_sets[f] = sets[f];

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ctx->applyAnisotropy(si, 16.0f);
    si.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(ctx->device(), &si, nullptr, &m_linearSampler);
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(ctx->device(), &si, nullptr, &m_nearestSampler);
    m_nearestSamplers.insert(m_nearestSampler);

    if (!createArray(m_array, VK_FORMAT_R8G8B8A8_UNORM, 32)) return false;
    if (!createCopyPipeline()) return false;

    m_table.assign(slotCount, 0u);
    m_places.assign(slotCount, Place{kNone, 0});
    for (uint32_t f = 0; f < kFrames; ++f) {
        if (!ctx->createBuffer(slotCount * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VMA_MEMORY_USAGE_CPU_TO_GPU, m_tableBuffer[f], m_tableAlloc[f]))
            return false;
        void* mapped = nullptr;
        vmaMapMemory(ctx->allocator(), m_tableAlloc[f], &mapped);
        m_tableMapped[f] = static_cast<uint32_t*>(mapped);
        std::memset(mapped, 0, slotCount * sizeof(uint32_t));

        VkDescriptorBufferInfo bi{m_tableBuffer[f], 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m_sets[f];
        w.dstBinding = kTableBinding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(ctx->device(), 1, &w, 0, nullptr);
        writeArrayBindings(m_sets[f]);
        writeDirectBindings(m_sets[f]);
    }

    // A camada 0 é a textura padrão: slot sem textura (tabela zerada) lê dela.
    m_jobs[kNone] = CopyJob{defaultView, defaultSampler, false, false};
    runCopies(SIZE_MAX);
    ERUPTION_LOG_WARN("Texture layers: %ux%u per texture, %u mips (GPU without descriptor indexing)",
                      m_layerSize, m_layerSize, m_mipLevels);
    return true;
}

void TextureLayers::shutdown() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    destroyArray(m_array);
    for (uint32_t f = 0; f < kFrames; ++f) {
        if (m_tableBuffer[f] == VK_NULL_HANDLE) continue;
        vmaUnmapMemory(m_ctx->allocator(), m_tableAlloc[f]);
        vmaDestroyBuffer(m_ctx->allocator(), m_tableBuffer[f], m_tableAlloc[f]);
        m_tableBuffer[f] = VK_NULL_HANDLE;
        m_tableMapped[f] = nullptr;
    }
    if (m_copyPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_copyPipeline, nullptr);
    m_copyPipeline = VK_NULL_HANDLE;
    if (m_copyLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, m_copyLayout, nullptr);
    if (m_copySetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, m_copySetLayout, nullptr);
    if (m_copyPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_copyPool, nullptr);
    m_copyLayout = VK_NULL_HANDLE;
    m_copySetLayout = VK_NULL_HANDLE;
    m_copyPool = VK_NULL_HANDLE;
    if (m_linearSampler != VK_NULL_HANDLE) vkDestroySampler(dev, m_linearSampler, nullptr);
    if (m_nearestSampler != VK_NULL_HANDLE) vkDestroySampler(dev, m_nearestSampler, nullptr);
    m_linearSampler = m_nearestSampler = VK_NULL_HANDLE;
    m_jobs.clear();
    m_table.clear();
    m_places.clear();
    m_nearestSamplers.clear();
    m_ctx = nullptr;
}

bool TextureLayers::createArray(LayerArray& arr, VkFormat format, uint32_t capacity) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {m_layerSize, m_layerSize, 1};
    ii.mipLevels = m_mipLevels;
    ii.arrayLayers = capacity;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_ctx->allocator(), &ii, &ai, &arr.image, &arr.alloc, nullptr) != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("Texture layers: could not create a %u-layer array", capacity);
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = arr.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, m_mipLevels, 0, capacity};
    vkc::createImageView(m_ctx->device(), &vi, nullptr, &arr.view);
    arr.format = format;
    arr.capacity = capacity;

    // Camadas ainda sem cópia ficam legíveis (lixo, mas nenhum slot aponta
    // para elas).
    const VkImage image = arr.image;
    const uint32_t mips = m_mipLevels;
    m_ctx->immediateSubmit([image, mips, capacity](VkCommandBuffer cmd) {
        VkImageMemoryBarrier b = layerBarrier(image, 0, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT);
        b.subresourceRange.layerCount = capacity;
        barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, b);
    });
    return true;
}

void TextureLayers::destroyArray(LayerArray& arr) {
    if (arr.view != VK_NULL_HANDLE) vkDestroyImageView(m_ctx->device(), arr.view, nullptr);
    if (arr.image != VK_NULL_HANDLE) vmaDestroyImage(m_ctx->allocator(), arr.image, arr.alloc);
    arr = LayerArray{};
}

// Array cheio: outro com o dobro de camadas, as antigas copiadas. Acontece na
// carga do mapa; espera a GPU para trocar a imagem em todos os sets.
bool TextureLayers::growArray() {
    LayerArray& arr = m_array;
    const uint32_t maxLayers = std::min(m_ctx->deviceProperties().limits.maxImageArrayLayers, 0xFFFFu);
    // Cresce 1/4 por vez, não o dobro: a matriz é a maior alocação do
    // celular (1024 camadas de 256² = 344 MB com ~960 em uso) e o dobro
    // pediria 688 MB mais a antiga viva durante a cópia.
    const uint32_t capacity = std::min(arr.capacity + std::max(32u, arr.capacity / 4), maxLayers);
    if (capacity <= arr.capacity) return false;
    vkDeviceWaitIdle(m_ctx->device());
    LayerArray bigger;
    if (!createArray(bigger, arr.format, capacity)) return false;

    // Só as camadas em uso, em fatias de kGrowChunk por envio: as ~760
    // camadas num envio só levaram 2,5 s na GE8320 e prenderam a GPU.
    const VkImage from = arr.image, to = bigger.image;
    const uint32_t used = arr.next, mips = m_mipLevels, size = m_layerSize;
    for (uint32_t base = 0; base < used; base += kGrowChunk) {
        const uint32_t count = std::min(kGrowChunk, used - base);
        m_ctx->immediateSubmit([from, to, base, count, mips, size](VkCommandBuffer cmd) {
            VkImageMemoryBarrier b[2] = {
                layerBarrier(from, base, 0, mips, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT),
                layerBarrier(to, base, 0, mips, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
            };
            b[0].subresourceRange.layerCount = b[1].subresourceRange.layerCount = count;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 2, b);
            std::vector<VkImageCopy> regions(mips);
            for (uint32_t m = 0; m < mips; ++m) {
                VkImageCopy& r = regions[m];
                r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, base, count};
                r.dstSubresource = r.srcSubresource;
                const uint32_t side = std::max(1u, size >> m);
                r.extent = {side, side, 1};
            }
            vkCmdCopyImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           mips, regions.data());
            b[0] = layerBarrier(from, base, 0, mips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, 0);
            b[1] = layerBarrier(to, base, 0, mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT);
            b[0].subresourceRange.layerCount = b[1].subresourceRange.layerCount = count;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                 nullptr, 0, nullptr, 2, b);
        });
    }
    bigger.next = arr.next;
    bigger.freeLayers = std::move(arr.freeLayers);
    destroyArray(arr);
    arr = std::move(bigger);
    for (VkDescriptorSet set : m_sets) writeArrayBindings(set);
    ERUPTION_LOG_INFO("Texture layers: array grown to %u layers", capacity);
    return true;
}

bool TextureLayers::createCopyPipeline() {
    VkDevice dev = m_ctx->device();
    VkDescriptorSetLayoutBinding src{};
    src.binding = 0;
    src.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    src.descriptorCount = 1;
    src.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings = &src;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_copySetLayout) != VK_SUCCESS) return false;
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_copySetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_copyLayout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kCopyBatch};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kCopyBatch;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_copyPool) != VK_SUCCESS) return false;

    VkShaderModule vert = loadModule(dev, "postprocess/fullscreen.vert.spv");
    VkShaderModule frag = loadModule(dev, "common/layer_copy.frag.spv");
    if (vert != VK_NULL_HANDLE && frag != VK_NULL_HANDLE) {
        std::vector<VkPipelineShaderStageCreateInfo> stages(2);
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[0].pName = stages[1].pName = "main";
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 0xF;
        m_copyPipeline = PipelineBuilder()
                             .setShaderStages(stages)
                             .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                             .setViewport(0, 0, static_cast<float>(m_layerSize), static_cast<float>(m_layerSize))
                             .setScissor(0, 0, m_layerSize, m_layerSize)
                             .setPolygonMode(VK_POLYGON_MODE_FILL)
                             .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                             .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
                             .setBlendState({blend})
                             .setLayout(m_copyLayout)
                             .setColorAttachmentFormats({m_array.format})
                             .build(dev);
    }
    if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(dev, vert, nullptr);
    if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(dev, frag, nullptr);
    return m_copyPipeline != VK_NULL_HANDLE;
}

uint32_t TextureLayers::allocLayer() {
    LayerArray& arr = m_array;
    if (!arr.freeLayers.empty()) {
        const uint32_t layer = arr.freeLayers.back();
        arr.freeLayers.pop_back();
        return layer;
    }
    if (arr.next >= arr.capacity && !growArray()) {
        ERUPTION_LOG_ERROR("Texture layers: array full (%u layers)", arr.capacity);
        return 0;
    }
    return arr.next++;
}

void TextureLayers::freePlace(uint32_t slot) {
    Place& p = m_places[slot];
    if (p.kind < kDirectKind) {
        if (p.layer != 0) m_array.freeLayers.push_back(p.layer);
    } else if (p.kind != kNone) {
        m_direct[p.kind - kDirectKind] = Direct{};
        for (bool& d : m_directDirty) d = true;
    }
    p = Place{kNone, 0};
}

void TextureLayers::setTable(uint32_t slot, uint32_t kind, uint32_t layer) {
    m_table[slot] = (kind << 16) | layer;
    for (bool& d : m_tableDirty) d = true;
}

void TextureLayers::assign(uint32_t slot, VkImageView view, VkSampler sampler, bool dynamic) {
    if (slot >= m_table.size()) return;
#if defined(__EMSCRIPTEN__)
    web::setSlotView(slot, view); // o desenho WebGL2 le a textura pelo slot
#endif
    const VkFormat format = vkc::viewFormat(view);
    if (!dynamic && copyable(format)) {
        m_jobs[slot] = CopyJob{view, sampler, m_nearestSamplers.count(sampler) != 0, isSrgb(format)};
        return;
    }
    m_jobs.erase(slot);
    Place& p = m_places[slot];
    uint32_t k = kDirectCount;
    if (p.kind >= kDirectKind && p.kind != kNone) {
        k = p.kind - kDirectKind;
    } else {
        freePlace(slot);
        for (uint32_t i = 0; i < kDirectCount && k == kDirectCount; ++i)
            if (m_direct[i].slot == UINT32_MAX) k = i;
    }
    if (k == kDirectCount) {
        if (!m_warnedDirectFull) {
            ERUPTION_LOG_WARN("Texture layers: all %u direct bindings in use; slot %u shows the default texture",
                              kDirectCount, slot);
            m_warnedDirectFull = true;
        }
        setTable(slot, 0, 0);
        return;
    }
    m_direct[k] = Direct{slot, view, sampler};
    p = Place{kDirectKind + k, 0};
    setTable(slot, kDirectKind + k, 0);
    for (bool& d : m_directDirty) d = true;
}

void TextureLayers::release(uint32_t slot) {
    if (slot >= m_table.size()) return;
    m_jobs.erase(slot);
    freePlace(slot);
    setTable(slot, 0, 0);
}

void TextureLayers::recordCopy(VkCommandBuffer cmd, VkDescriptorSet copySet, uint32_t layer, bool srgb,
                               VkImageView layerView) {
    const VkImage image = m_array.image;
    const int32_t size = static_cast<int32_t>(m_layerSize);
    barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            layerBarrier(image, layer, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT));

    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = layerView;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    m_ctx->cmdBeginRendering(cmd, {att}, nullptr, nullptr, {m_layerSize, m_layerSize});
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_copyPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_copyLayout, 0, 1, &copySet, 0, nullptr);
    const uint32_t encodeSrgb = srgb ? 1u : 0u;
    vkCmdPushConstants(cmd, m_copyLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(encodeSrgb), &encodeSrgb);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    m_ctx->cmdEndRendering(cmd);

    // Mips por blit, cada nível a partir do anterior.
    barrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            layerBarrier(image, layer, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT));
    for (uint32_t m = 1; m < m_mipLevels; ++m) {
        barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                layerBarrier(image, layer, m, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                             VK_ACCESS_TRANSFER_WRITE_BIT));
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, layer, 1};
        blit.srcOffsets[1] = {std::max(1, size >> (m - 1)), std::max(1, size >> (m - 1)), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, layer, 1};
        blit.dstOffsets[1] = {std::max(1, size >> m), std::max(1, size >> m), 1};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
        barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                layerBarrier(image, layer, m, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT));
    }
    barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            layerBarrier(image, layer, 0, m_mipLevels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_ACCESS_SHADER_READ_BIT));
}

void TextureLayers::runCopies(size_t maxCopies) {
    const auto started = std::chrono::steady_clock::now();
    struct Target {
        uint32_t slot;
        uint32_t layer;
        CopyJob job;
    };
    std::vector<Target> targets;
    targets.reserve(std::min(maxCopies, m_jobs.size()));
    for (auto it = m_jobs.begin(); it != m_jobs.end() && targets.size() < maxCopies; it = m_jobs.erase(it)) {
        const uint32_t slot = it->first;
        const CopyJob& job = it->second;
        if (slot == kNone) {
            targets.push_back({kNone, 0, job}); // textura padrão: camada 0
            continue;
        }
        const Place p = m_places[slot];
        uint32_t layer;
        if (p.kind < kDirectKind && p.layer != 0) {
            layer = p.layer; // mesma textura trocada: reaproveita a camada
        } else {
            freePlace(slot);
            layer = allocLayer();
            if (layer == 0) continue;
        }
        targets.push_back({slot, layer, job});
    }

    VkDevice dev = m_ctx->device();
    for (size_t first = 0; first < targets.size(); first += kCopyBatch) {
        const size_t count = std::min<size_t>(kCopyBatch, targets.size() - first);
        vkResetDescriptorPool(dev, m_copyPool, 0);
        std::vector<VkDescriptorSetLayout> layouts(count, m_copySetLayout);
        std::vector<VkDescriptorSet> sets(count);
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_copyPool;
        ai.descriptorSetCount = static_cast<uint32_t>(count);
        ai.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(dev, &ai, sets.data()) != VK_SUCCESS) {
            ERUPTION_LOG_ERROR("Texture layers: copy descriptor sets failed");
            return;
        }
        std::vector<VkImageView> layerViews(count);
        for (size_t i = 0; i < count; ++i) {
            const Target& t = targets[first + i];
            VkDescriptorImageInfo ii{t.job.nearest ? m_nearestSampler : m_linearSampler, t.job.view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = sets[i];
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &ii;
            vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);

            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = m_array.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = m_array.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, t.layer, 1};
            vkc::createImageView(dev, &vi, nullptr, &layerViews[i]);
        }
        m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
            for (size_t i = 0; i < count; ++i) {
                const Target& t = targets[first + i];
                recordCopy(cmd, sets[i], t.layer, t.job.srgb, layerViews[i]);
            }
        });
        for (VkImageView v : layerViews) vkDestroyImageView(dev, v, nullptr);
    }

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    ERUPTION_LOG_WARN("Texture layers: %zu copies in %.1f ms (%u layers used, %zu waiting)", targets.size(), ms,
                      m_array.next, m_jobs.size());
    for (const Target& t : targets) {
        if (t.slot == kNone) continue;
        const uint32_t kind = (t.job.srgb ? 1u : 0u) | (t.job.nearest ? 2u : 0u);
        m_places[t.slot] = Place{kind, t.layer};
        setTable(t.slot, kind, t.layer);
    }
}

void TextureLayers::writeArrayBindings(VkDescriptorSet set) {
    const VkDescriptorImageInfo infos[2] = {
        {m_linearSampler, m_array.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {m_nearestSampler, m_array.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = kLayersBinding + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_ctx->device(), 2, writes, 0, nullptr);
}

void TextureLayers::writeDirectBindings(VkDescriptorSet set) {
    VkDescriptorImageInfo infos[kDirectCount];
    VkWriteDescriptorSet writes[kDirectCount] = {};
    for (uint32_t k = 0; k < kDirectCount; ++k) {
        const Direct& d = m_direct[k];
        const bool used = d.slot != UINT32_MAX;
        infos[k] = {used ? d.sampler : m_defaultSampler, used ? d.view : m_defaultView,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[k].dstSet = set;
        writes[k].dstBinding = kDirectBinding + k;
        writes[k].descriptorCount = 1;
        writes[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[k].pImageInfo = &infos[k];
    }
    vkUpdateDescriptorSets(m_ctx->device(), kDirectCount, writes, 0, nullptr);
}

void TextureLayers::apply(uint32_t frame) {
    if (!m_jobs.empty()) {
        const auto t0 = std::chrono::steady_clock::now();
        runCopies(kCopiesPerFrame);
        while (!m_jobs.empty() &&
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() < kCopyBudgetMs)
            runCopies(kCopyBatch);
    }
    if (m_tableDirty[frame]) {
        std::memcpy(m_tableMapped[frame], m_table.data(), m_table.size() * sizeof(uint32_t));
        m_tableDirty[frame] = false;
    }
    if (m_directDirty[frame]) {
        writeDirectBindings(m_sets[frame]);
        m_directDirty[frame] = false;
    }
}

void TextureLayers::applyAll() {
    if (!m_jobs.empty()) runCopies(SIZE_MAX);
    vkDeviceWaitIdle(m_ctx->device());
    for (uint32_t f = 0; f < kFrames; ++f) apply(f);
}

} // namespace eruption
