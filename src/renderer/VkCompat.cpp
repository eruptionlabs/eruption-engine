#include "renderer/VkCompat.hpp"

#include "core/Logger.hpp"
#include "renderer/BandwidthMeter.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace eruption::vkc {

namespace {

VkDevice g_device = VK_NULL_HANDLE;
std::mutex g_mutex;
std::unordered_map<VkImageView, ViewInfo> g_views;
std::unordered_map<std::string, VkRenderPass> g_renderPasses;
// Framebuffers reaproveitados entre quadros, pela combinacao de render pass,
// views e tamanho. Criar e destruir um por passe a cada quadro custava ~40% da
// CPU no driver de GPU de celular (registro do alvo no kernel).
struct CachedFramebuffer {
    VkFramebuffer fb = VK_NULL_HANDLE;
    uint64_t lastUse = 0;
    std::vector<VkImageView> views;
};
std::unordered_map<std::string, CachedFramebuffer> g_framebuffers;
uint64_t g_frameNumber = 0;
uint32_t g_framesInFlight = 0;
PipelineHook g_pipelineHook = nullptr;
void* g_pipelineHookUser = nullptr;

struct AttachmentDesc {
    VkFormat format;
    VkAttachmentLoadOp load;
    VkAttachmentStoreOp store;
    VkAttachmentLoadOp stencilLoad;
    VkAttachmentStoreOp stencilStore;
    VkImageLayout layout;
};

bool hasStencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT ||
           f == VK_FORMAT_S8_UINT;
}

// STORE_OP_NONE (attachment so' lido no passe) so' existe na render pass
// classica com VK_KHR_load_store_op_none; sem ela, STORE guarda o mesmo
// conteudo. DONT_CARE nao serve: descartaria a profundidade que o passe le'.
VkAttachmentStoreOp classicStoreOp(VkAttachmentStoreOp op) {
    return op == VK_ATTACHMENT_STORE_OP_NONE ? VK_ATTACHMENT_STORE_OP_STORE : op;
}

// Cria (ou acha no cache) o render pass de uma subpassagem com estes anexos.
// O último é profundidade/stencil quando `hasDepth`.
VkRenderPass renderPassFor(const std::vector<AttachmentDesc>& atts, size_t colorCount, bool hasDepth) {
    std::string key(reinterpret_cast<const char*>(atts.data()), atts.size() * sizeof(AttachmentDesc));
    key += static_cast<char>(colorCount);
    key += hasDepth ? 'd' : 'n';
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_renderPasses.find(key);
    if (it != g_renderPasses.end()) return it->second;

    std::vector<VkAttachmentDescription> desc(atts.size());
    std::vector<VkAttachmentReference> colorRefs(colorCount);
    VkAttachmentReference depthRef{};
    for (size_t i = 0; i < atts.size(); ++i) {
        VkAttachmentDescription& d = desc[i];
        d.format = atts[i].format;
        d.samples = VK_SAMPLE_COUNT_1_BIT;
        d.loadOp = atts[i].load;
        d.storeOp = classicStoreOp(atts[i].store);
        d.stencilLoadOp = atts[i].stencilLoad;
        d.stencilStoreOp = classicStoreOp(atts[i].stencilStore);
        // Sem transição dentro do passe: as barreiras do motor já deixaram a
        // imagem no layout de uso, como no dynamic rendering.
        d.initialLayout = atts[i].layout;
        d.finalLayout = atts[i].layout;
        if (i < colorCount) colorRefs[i] = {static_cast<uint32_t>(i), atts[i].layout};
        else depthRef = {static_cast<uint32_t>(i), atts[i].layout};
    }
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = static_cast<uint32_t>(colorCount);
    sub.pColorAttachments = colorRefs.data();
    sub.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = static_cast<uint32_t>(desc.size());
    ci.pAttachments = desc.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(g_device, &ci, nullptr, &rp) != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("vkc: falha ao criar render pass (%zu anexos)", atts.size());
        return VK_NULL_HANDLE;
    }
    g_renderPasses.emplace(std::move(key), rp);
    return rp;
}

// synchronization2 -> 1.1: os bits antigos têm o mesmo valor; os novos viram
// o estágio/acesso antigo que os contém.
VkPipelineStageFlags stageV1(VkPipelineStageFlags2 s, bool src) {
    VkPipelineStageFlags out = static_cast<VkPipelineStageFlags>(s & 0x7FFFFFFFull);
    if (s & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
             VK_PIPELINE_STAGE_2_CLEAR_BIT))
        out |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (s & (VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT))
        out |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (s & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT)
        out |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
               VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    if (out == 0) out = src ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    return out;
}

VkAccessFlags accessV1(VkAccessFlags2 a) {
    VkAccessFlags out = static_cast<VkAccessFlags>(a & 0x7FFFFFFFull);
    if (a & (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) out |= VK_ACCESS_SHADER_READ_BIT;
    if (a & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) out |= VK_ACCESS_SHADER_WRITE_BIT;
    return out;
}

} // namespace

VkFormat viewFormat(VkImageView view) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_views.find(view);
    return it == g_views.end() ? VK_FORMAT_UNDEFINED : it->second.format;
}

bool viewInfo(VkImageView view, ViewInfo& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_views.find(view);
    if (it == g_views.end()) return false;
    out = it->second;
    return true;
}

void init(VkDevice device, uint32_t framesInFlight) {
    g_device = device;
    g_framebuffers.clear();
    g_frameNumber = 0;
    g_framesInFlight = framesInFlight;
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [key, cached] : g_framebuffers) vkDestroyFramebuffer(g_device, cached.fb, nullptr);
    g_framebuffers.clear();
    for (auto& [k, rp] : g_renderPasses) vkDestroyRenderPass(g_device, rp, nullptr);
    g_renderPasses.clear();
    g_views.clear();
    g_device = VK_NULL_HANDLE;
}

void beginFrame(uint32_t /*frameIndex*/) {
    // Medidor de banda: fecha o quadro anterior (spec 50).
    bw::beginFrame();
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_frameNumber;
    // Sem uso ha' mais quadros do que os em voo: nenhum comando pendente o usa.
    const uint64_t keep = g_framesInFlight + 1u;
    for (auto it = g_framebuffers.begin(); it != g_framebuffers.end();) {
        if (it->second.lastUse + keep < g_frameNumber) {
            vkDestroyFramebuffer(g_device, it->second.fb, nullptr);
            it = g_framebuffers.erase(it);
        } else {
            ++it;
        }
    }
}

// Uma view nova pode reusar o handle de uma destruida: os framebuffers que
// apontavam para a antiga nao servem mais (e quem destruiu a view ja' esperou
// a GPU, entao eles tambem estao livres).
static void forgetFramebuffersOf(VkImageView view) {
    for (auto it = g_framebuffers.begin(); it != g_framebuffers.end();) {
        const auto& v = it->second.views;
        if (std::find(v.begin(), v.end(), view) != v.end()) {
            vkDestroyFramebuffer(g_device, it->second.fb, nullptr);
            it = g_framebuffers.erase(it);
        } else {
            ++it;
        }
    }
}

VkResult createImageView(VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* alloc,
                         VkImageView* view) {
    const VkResult r = vkCreateImageView(device, info, alloc, view);
    if (r == VK_SUCCESS) {
        bw::noteView(*view, info->image);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_views[*view] = ViewInfo{info->image, info->viewType, info->format, info->subresourceRange};
        forgetFramebuffersOf(*view);
    }
    return r;
}

VkRenderPass compatibleRenderPass(const std::vector<VkFormat>& colors, VkFormat depth, VkFormat stencil) {
    std::vector<AttachmentDesc> atts;
    for (VkFormat f : colors)
        atts.push_back({f, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    const VkFormat ds = depth != VK_FORMAT_UNDEFINED ? depth : stencil;
    if (ds != VK_FORMAT_UNDEFINED)
        atts.push_back({ds, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});
    return renderPassFor(atts, colors.size(), ds != VK_FORMAT_UNDEFINED);
}

VkResult createGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                 const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc,
                                 VkPipeline* pipelines) {
    // Troca a descrição de dynamic rendering (pNext) por um render pass
    // compatível com os mesmos formatos.
    std::vector<VkGraphicsPipelineCreateInfo> fixed(infos, infos + count);
    for (auto& ci : fixed) {
        if (ci.renderPass != VK_NULL_HANDLE) continue;
        const VkBaseInStructure* prev = nullptr;
        const VkBaseInStructure* node = static_cast<const VkBaseInStructure*>(ci.pNext);
        while (node && node->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            prev = node;
            node = node->pNext;
        }
        if (!node) continue;
        const auto* ri = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(node);
        std::vector<VkFormat> colors(ri->pColorAttachmentFormats, ri->pColorAttachmentFormats + ri->colorAttachmentCount);
        ci.renderPass = compatibleRenderPass(colors, ri->depthAttachmentFormat, ri->stencilAttachmentFormat);
        ci.subpass = 0;
        // Tira a estrutura da cadeia (ela não existe no Vulkan 1.1).
        if (!prev) ci.pNext = node->pNext;
        else ERUPTION_LOG_WARN("vkc: VkPipelineRenderingCreateInfo no meio da cadeia pNext; mantida");
    }
    const VkResult r = vkCreateGraphicsPipelines(device, cache, count, fixed.data(), alloc, pipelines);
    // Driver de celular recusa pipeline que o do PC aceita: o código de erro
    // no log é o ponto de partida.
    if (r != VK_SUCCESS) ERUPTION_LOG_ERROR("vkc: vkCreateGraphicsPipelines failed (%d)", static_cast<int>(r));
    if (g_pipelineHook) g_pipelineHook(g_pipelineHookUser);
    return r;
}

PipelineHookScope::PipelineHookScope(PipelineHook hook, void* user)
    : m_prevHook(g_pipelineHook), m_prevUser(g_pipelineHookUser) {
    g_pipelineHook = hook;
    g_pipelineHookUser = user;
}

PipelineHookScope::~PipelineHookScope() {
    g_pipelineHook = m_prevHook;
    g_pipelineHookUser = m_prevUser;
}

static VkFramebuffer framebufferFor(const VkFramebufferCreateInfo& fi, const std::vector<VkImageView>& views) {
    std::string key(reinterpret_cast<const char*>(&fi.renderPass), sizeof(fi.renderPass));
    key.append(reinterpret_cast<const char*>(&fi.width), sizeof(fi.width));
    key.append(reinterpret_cast<const char*>(&fi.height), sizeof(fi.height));
    key.append(reinterpret_cast<const char*>(&fi.layers), sizeof(fi.layers));
    key.append(reinterpret_cast<const char*>(views.data()), views.size() * sizeof(VkImageView));
    std::lock_guard<std::mutex> lock(g_mutex);
    CachedFramebuffer& cached = g_framebuffers[key];
    if (cached.fb == VK_NULL_HANDLE) {
        vkCreateFramebuffer(g_device, &fi, nullptr, &cached.fb);
        cached.views = views;
    }
    cached.lastUse = g_frameNumber;
    return cached.fb;
}

void cmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info) {
    // Todo passe do motor passa por aqui: o medidor de banda conta os anexos.
    bw::recordRendering(*info, &viewFormat);
    std::vector<AttachmentDesc> atts;
    std::vector<VkImageView> views;
    std::vector<VkClearValue> clears;
    size_t colorCount = 0;
    for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
        const VkRenderingAttachmentInfo& a = info->pColorAttachments[i];
        if (a.imageView == VK_NULL_HANDLE) continue;
        atts.push_back({viewFormat(a.imageView), a.loadOp, a.storeOp, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        VK_ATTACHMENT_STORE_OP_DONT_CARE, a.imageLayout});
        views.push_back(a.imageView);
        clears.push_back(a.clearValue);
        ++colorCount;
    }
    const VkRenderingAttachmentInfo* d = info->pDepthAttachment && info->pDepthAttachment->imageView ? info->pDepthAttachment : nullptr;
    const VkRenderingAttachmentInfo* s = info->pStencilAttachment && info->pStencilAttachment->imageView ? info->pStencilAttachment : nullptr;
    const VkRenderingAttachmentInfo* ds = d ? d : s;
    if (ds) {
        const VkFormat f = viewFormat(ds->imageView);
        const bool st = hasStencil(f);
        atts.push_back({f, d ? d->loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE, d ? d->storeOp : VK_ATTACHMENT_STORE_OP_DONT_CARE,
                        (st && s) ? s->loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        (st && s) ? s->storeOp : VK_ATTACHMENT_STORE_OP_DONT_CARE, ds->imageLayout});
        views.push_back(ds->imageView);
        clears.push_back(ds->clearValue);
    }
    const VkRenderPass rp = renderPassFor(atts, colorCount, ds != nullptr);

    VkFramebufferCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = rp;
    fi.attachmentCount = static_cast<uint32_t>(views.size());
    fi.pAttachments = views.data();
    fi.width = static_cast<uint32_t>(info->renderArea.offset.x) + info->renderArea.extent.width;
    fi.height = static_cast<uint32_t>(info->renderArea.offset.y) + info->renderArea.extent.height;
    fi.layers = info->layerCount ? info->layerCount : 1;
    const VkFramebuffer fb = framebufferFor(fi, views);

    VkRenderPassBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea = info->renderArea;
    bi.clearValueCount = static_cast<uint32_t>(clears.size());
    bi.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

void cmdEndRendering(VkCommandBuffer cmd) {
    vkCmdEndRenderPass(cmd);
}

void cmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* dep) {
    VkPipelineStageFlags2 src = 0, dst = 0;
    std::vector<VkMemoryBarrier> mem(dep->memoryBarrierCount);
    for (uint32_t i = 0; i < dep->memoryBarrierCount; ++i) {
        const auto& b = dep->pMemoryBarriers[i];
        mem[i] = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, accessV1(b.srcAccessMask), accessV1(b.dstAccessMask)};
        src |= b.srcStageMask;
        dst |= b.dstStageMask;
    }
    std::vector<VkBufferMemoryBarrier> buf(dep->bufferMemoryBarrierCount);
    for (uint32_t i = 0; i < dep->bufferMemoryBarrierCount; ++i) {
        const auto& b = dep->pBufferMemoryBarriers[i];
        buf[i] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr, accessV1(b.srcAccessMask), accessV1(b.dstAccessMask),
                  b.srcQueueFamilyIndex, b.dstQueueFamilyIndex, b.buffer, b.offset, b.size};
        src |= b.srcStageMask;
        dst |= b.dstStageMask;
    }
    std::vector<VkImageMemoryBarrier> img(dep->imageMemoryBarrierCount);
    for (uint32_t i = 0; i < dep->imageMemoryBarrierCount; ++i) {
        const auto& b = dep->pImageMemoryBarriers[i];
        img[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, accessV1(b.srcAccessMask), accessV1(b.dstAccessMask),
                  b.oldLayout, b.newLayout, b.srcQueueFamilyIndex, b.dstQueueFamilyIndex, b.image, b.subresourceRange};
        src |= b.srcStageMask;
        dst |= b.dstStageMask;
    }
    vkCmdPipelineBarrier(cmd, stageV1(src, true), stageV1(dst, false), dep->dependencyFlags,
                         static_cast<uint32_t>(mem.size()), mem.data(), static_cast<uint32_t>(buf.size()), buf.data(),
                         static_cast<uint32_t>(img.size()), img.data());
}

} // namespace eruption::vkc
