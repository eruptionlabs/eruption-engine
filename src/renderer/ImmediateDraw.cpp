#include "renderer/ImmediateDraw.hpp"
#include "renderer/MipmapGenerator.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "utils/ImageUtils.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace eruption {

namespace {
constexpr float kSpriteCornerU[4] = {0.0f, 1.0f, 1.0f, 0.0f};
constexpr float kSpriteCornerV[4] = {1.0f, 1.0f, 0.0f, 0.0f};
constexpr float kSpriteSx[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
constexpr float kSpriteSy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
} // namespace

bool ImmediateDraw::init(VulkanContext* ctx, VkFormat colorFormat, VkFormat depthFormat) {
    m_ctx = ctx;
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
    VkDevice dev = ctx->device();

    std::vector<uint32_t> vertCode, fragCode;
    if (!ShaderCompiler::compileOrLoad("shaders/immediate/draw.vert", "shaders/immediate/draw.vert.spv",
                                       ShaderStage::Vertex, vertCode) ||
        !ShaderCompiler::compileOrLoad("shaders/immediate/draw.frag", "shaders/immediate/draw.frag.spv",
                                       ShaderStage::Fragment, fragCode)) {
        ERUPTION_LOG_ERROR("ImmediateDraw: shaders nao compilaram");
        return false;
    }
    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = vertCode.size() * sizeof(uint32_t);
    sm.pCode = vertCode.data();
    if (vkCreateShaderModule(dev, &sm, nullptr, &m_vert) != VK_SUCCESS) return false;
    sm.codeSize = fragCode.size() * sizeof(uint32_t);
    sm.pCode = fragCode.data();
    if (vkCreateShaderModule(dev, &sm, nullptr, &m_frag) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding bind{};
    bind.binding = 0;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl{};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 1;
    dl.pBindings = &bind;
    if (vkCreateDescriptorSetLayout(dev, &dl, nullptr, &m_setLayout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kMaxTextures;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;

    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)}; // 64 bytes
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 1;
    li.pSetLayouts = &m_setLayout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &m_layout) != VK_SUCCESS) return false;

    // REPEAT: o U de um disco "rolando" passa de 1. Linear + mip linear, sem anisotropia.
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(dev, &si, nullptr, &m_sampler) != VK_SUCCESS) return false;

    // Anel: um par vertices/indices por quadro em voo, mapeado de forma
    // persistente. Nada e' alocado depois daqui.
    for (auto& f : m_frames) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        bi.size = VkDeviceSize(kMaxVertices) * sizeof(Vertex);
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (vmaCreateBuffer(ctx->allocator(), &bi, &ai, &f.vb, &f.vbAlloc, &info) != VK_SUCCESS) return false;
        f.vbMap = info.pMappedData;
        bi.size = VkDeviceSize(kMaxIndices) * sizeof(uint16_t);
        bi.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (vmaCreateBuffer(ctx->allocator(), &bi, &ai, &f.ib, &f.ibAlloc, &info) != VK_SUCCESS) return false;
        f.ibMap = info.pMappedData;
        if (!f.vbMap || !f.ibMap) return false;
    }
    m_verts.reserve(4096);
    m_cmds.reserve(256);
    m_ready = true;
    return true;
}

void ImmediateDraw::destroyTexture(Texture& t) {
    VkDevice dev = m_ctx->device();
    if (t.view) vkDestroyImageView(dev, t.view, nullptr);
    if (t.image) vmaDestroyImage(m_ctx->allocator(), t.image, t.alloc);
    t = Texture{};
}

void ImmediateDraw::shutdown() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    vkDeviceWaitIdle(dev);
    for (auto& t : m_textures) destroyTexture(t);
    m_textures.clear();
    m_texIds.clear();
    for (VkPipeline& p : m_pipes) {
        if (p) vkDestroyPipeline(dev, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    for (auto& f : m_frames) {
        if (f.vb) vmaDestroyBuffer(m_ctx->allocator(), f.vb, f.vbAlloc);
        if (f.ib) vmaDestroyBuffer(m_ctx->allocator(), f.ib, f.ibAlloc);
        f = FrameBuf{};
    }
    if (m_sampler) vkDestroySampler(dev, m_sampler, nullptr);
    if (m_pool) vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_layout) vkDestroyPipelineLayout(dev, m_layout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_vert) vkDestroyShaderModule(dev, m_vert, nullptr);
    if (m_frag) vkDestroyShaderModule(dev, m_frag, nullptr);
    m_sampler = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_vert = m_frag = VK_NULL_HANDLE;
    discard();
    m_ready = false;
}

bool ImmediateDraw::loadTexture(const std::string& path, Texture& out) {
    int w = 0, h = 0, ch = 0;
    uint8_t* px = ImageUtils::loadRawVfs(project::gameRef(path).string(), w, h, ch, 4);
    if (!px) return false;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        stbi_image_free(px);
        return false;
    }
    const bool ok = uploadTexture(px, w, h, out);
    stbi_image_free(px);
    return ok;
}

// Envia pixels RGBA8 ja' decodificados (arquivo ou procedurais) e monta o descritor.
bool ImmediateDraw::uploadTexture(const uint8_t* px, int w, int h, Texture& out) {
    const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    const uint32_t mips = MipmapGenerator::calculateMipLevels(uint32_t(w), uint32_t(h));
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    if (!m_ctx->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc)) {
        return false;
    }
    void* mapped = nullptr;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, px, size_t(bytes));
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    if (!m_ctx->createImage(uint32_t(w), uint32_t(h), format,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY, out.image, out.alloc, mips)) {
        vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);
        return false;
    }
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        m_ctx->cmdImageBarrier(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {uint32_t(w), uint32_t(h), 1};
        vkCmdCopyBufferToImage(cmd, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        if (mips > 1) {
            MipmapGenerator::generateMipmapsBlit(cmd, out.image, uint32_t(w), uint32_t(h), mips);
        } else {
            m_ctx->cmdImageBarrier(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT);
        }
    });
    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    if (vkCreateImageView(m_ctx->device(), &vi, nullptr, &out.view) != VK_SUCCESS) {
        destroyTexture(out);
        return false;
    }
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(m_ctx->device(), &ai, &out.set) != VK_SUCCESS) {
        destroyTexture(out);
        return false;
    }
    VkDescriptorImageInfo ii{m_sampler, out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = out.set;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &wr, 0, nullptr);
    return true;
}

int ImmediateDraw::textureId(const std::string& path) {
    if (!m_ready) return -1;
    auto it = m_texIds.find(path);
    if (it != m_texIds.end()) return it->second;
    int id = -1;
    if (m_textures.size() >= kMaxTextures) {
        ERUPTION_LOG_WARN("ImmediateDraw: mais de %u texturas diferentes, '%s' ignorada", kMaxTextures, path.c_str());
    } else {
        Texture t;
        if (loadTexture(path, t)) {
            id = int(m_textures.size());
            m_textures.push_back(t);
        } else {
            ERUPTION_LOG_WARN("ImmediateDraw: textura '%s' nao abriu", path.c_str());
        }
    }
    m_texIds.emplace(path, id); // a falha tambem fica em cache: sem reabrir todo quadro
    return id;
}

int ImmediateDraw::addTexture(const std::string& key, const uint8_t* rgba, int w, int h) {
    if (!m_ready) return -1;
    auto it = m_texIds.find(key);
    if (it != m_texIds.end()) return it->second;
    int id = -1;
    if (m_textures.size() < kMaxTextures && w > 0 && h > 0) {
        Texture t;
        if (uploadTexture(rgba, w, h, t)) {
            id = int(m_textures.size());
            m_textures.push_back(t);
        }
    }
    m_texIds.emplace(key, id);
    return id;
}

void ImmediateDraw::warnOnce(const char* what) {
    if (m_warned) return;
    m_warned = true;
    ERUPTION_LOG_WARN("ImmediateDraw: %s (teto de %u vertices por quadro); o excesso e' descartado", what, kMaxVertices);
}

ImmediateDraw::Vertex* ImmediateDraw::addGeometry(int tex, Topology topo, Flags flags, uint32_t count) {
    if (!m_ready || tex < 0) return nullptr;
    if (topo == Topology::Quads) count &= ~3u;
    if (count < (topo == Topology::Quads ? 4u : 3u)) return nullptr;
    if (m_pending + count > kMaxVertices) {
        warnOnce("quadro cheio");
        return nullptr;
    }
    const uint32_t first = uint32_t(m_verts.size());
    m_verts.resize(m_verts.size() + count);
    m_cmds.push_back({tex, packFlags(flags), uint8_t(topo == Topology::Quads ? 1 : 0), first, count});
    m_pending += count;
    return m_verts.data() + first;
}

void ImmediateDraw::addSprite(int tex, const Vec3& pos, float w, float h, const Vec4& rgba, Flags flags, bool pixels,
                              float rot) {
    if (!m_ready || tex < 0) return;
    if (m_pending + 4 > kMaxVertices) {
        warnOnce("quadro cheio");
        return;
    }
    const uint32_t first = uint32_t(m_sprites.size());
    m_sprites.push_back({pos, w, h, rot, rgba, pixels});
    m_cmds.push_back({tex, packFlags(flags), 2, first, 4});
    m_pending += 4;
}

VkPipeline ImmediateDraw::pipelineFor(uint8_t flags) {
    VkPipeline& slot = m_pipes[flags & 7];
    if (slot) return slot;
    const bool alpha = flags & 1, depthTest = flags & 2, cull = flags & 4;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r)},
    };
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &binding;
    vin.vertexAttributeDescriptionCount = 3;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    // O alfa da imagem iluminada nao e' do efeito: fica como estava.
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;

    // Profundidade so' de leitura (a da cena esta em DEPTH_STENCIL_READ_ONLY):
    // depth_write nao existe; com depth_test falso o efeito aparece por cima de tudo.
    slot = PipelineBuilder()
               .setShaderStages({stages[0], stages[1]})
               .setVertexInput(vin)
               .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
               .setPolygonMode(VK_POLYGON_MODE_FILL)
               .setCullMode(cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
               .setDepthState(depthTest, false, VK_COMPARE_OP_LESS_OR_EQUAL)
               .setBlendState({blend})
               .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
               .setLayout(m_layout)
               .setColorAttachmentFormats({m_colorFormat})
               .setDepthAttachmentFormat(m_depthFormat)
               .build(m_ctx->device());
    if (!slot) ERUPTION_LOG_ERROR("ImmediateDraw: pipeline %u nao foi criado", unsigned(flags));
    return slot;
}

bool ImmediateDraw::render(VkCommandBuffer cmd, const View& v) {
    struct Batch { int tex; uint8_t flags; uint32_t firstIndex, indexCount; };
    static thread_local std::vector<Batch> batches;
    batches.clear();
    m_lastDraws = m_lastVertices = 0;
    if (!m_ready || m_cmds.empty()) {
        discard();
        m_pending = 0;
        return false;
    }

    // Ordem: o aditivo e' comutativo, entao uma corrida de comandos aditivos
    // seguidos e' ordenada por (textura, flags) e vira poucos draws; o alfa
    // mantem a ordem da chamada (e quebra a corrida).
    m_order.resize(m_cmds.size());
    for (uint32_t i = 0; i < m_order.size(); ++i) m_order[i] = i;
    for (size_t i = 0; i < m_order.size();) {
        if (m_cmds[m_order[i]].flags & 1) { ++i; continue; }
        size_t j = i;
        while (j < m_order.size() && !(m_cmds[m_order[j]].flags & 1)) ++j;
        std::stable_sort(m_order.begin() + i, m_order.begin() + j, [this](uint32_t a, uint32_t b) {
            const Cmd& ca = m_cmds[a];
            const Cmd& cb = m_cmds[b];
            if (ca.tex != cb.tex) return ca.tex < cb.tex;
            return ca.flags < cb.flags;
        });
        i = j;
    }

    FrameBuf& fb = m_frames[m_ctx->currentFrame() % VulkanContext::MAX_FRAMES_IN_FLIGHT];
    Vertex* vout = static_cast<Vertex*>(fb.vbMap);
    uint16_t* iout = static_cast<uint16_t*>(fb.ibMap);
    uint32_t nv = 0, ni = 0;

    // Tamanho de 1 pixel de tela em unidades do mundo a uma profundidade de vista `w`.
    const float pxScale = 2.0f / (std::max(std::fabs(v.proj11), 1e-4f) * std::max(v.screenHeightPx, 1.0f));
    const Mat4& vp = v.viewProj;
    for (uint32_t oi : m_order) {
        const Cmd& c = m_cmds[oi];
        const uint32_t need = c.kind == 2 ? 4u : c.count;
        if (nv + need > kMaxVertices) continue;
        const uint32_t base = nv;
        uint32_t idxStart = ni;
        if (c.kind == 2) {
            const SpriteCmd& s = m_sprites[c.first];
            float hw = s.w * 0.5f, hh = s.h * 0.5f;
            if (s.pixels) {
                // clip.w = profundidade de vista (projecao em perspectiva)
                const float wv = vp[0][3] * s.pos.x + vp[1][3] * s.pos.y + vp[2][3] * s.pos.z + vp[3][3];
                const float k = pxScale * std::max(wv, 0.0f);
                hw *= k;
                hh *= k;
            }
            const float cr = std::cos(s.rot), sr = std::sin(s.rot);
            for (int k = 0; k < 4; ++k) {
                const float ox = kSpriteSx[k] * hw, oy = kSpriteSy[k] * hh;
                const float rx = ox * cr - oy * sr, ry = ox * sr + oy * cr;
                const Vec3 p = s.pos + v.camRight * rx + v.camUp * ry;
                vout[nv++] = {p.x, p.y, p.z, kSpriteCornerU[k], kSpriteCornerV[k], s.rgba.r, s.rgba.g, s.rgba.b, s.rgba.a};
            }
            const uint16_t b = uint16_t(base);
            iout[ni++] = b; iout[ni++] = uint16_t(b + 1); iout[ni++] = uint16_t(b + 2);
            iout[ni++] = b; iout[ni++] = uint16_t(b + 2); iout[ni++] = uint16_t(b + 3);
        } else {
            std::memcpy(vout + nv, m_verts.data() + c.first, size_t(c.count) * sizeof(Vertex));
            nv += c.count;
            if (c.kind == 1) {
                for (uint32_t q = 0; q + 4 <= c.count; q += 4) {
                    const uint16_t b = uint16_t(base + q);
                    iout[ni++] = b; iout[ni++] = uint16_t(b + 1); iout[ni++] = uint16_t(b + 2);
                    iout[ni++] = b; iout[ni++] = uint16_t(b + 2); iout[ni++] = uint16_t(b + 3);
                }
            } else {
                // Strip -> triangulos, o sentido alternando para a face da frente nao virar.
                for (uint32_t t = 0; t + 2 < c.count; ++t) {
                    const uint16_t a = uint16_t(base + t), b = uint16_t(base + t + 1), d = uint16_t(base + t + 2);
                    if (t & 1) { iout[ni++] = b; iout[ni++] = a; iout[ni++] = d; }
                    else       { iout[ni++] = a; iout[ni++] = b; iout[ni++] = d; }
                }
            }
        }
        if (!batches.empty() && batches.back().tex == c.tex && batches.back().flags == c.flags &&
            batches.back().firstIndex + batches.back().indexCount == idxStart)
            batches.back().indexCount += ni - idxStart;
        else
            batches.push_back({c.tex, c.flags, idxStart, ni - idxStart});
    }
    m_cmds.clear();
    m_verts.clear();
    m_sprites.clear();
    m_pending = 0;
    if (batches.empty() || ni == 0) return false;
    vmaFlushAllocation(m_ctx->allocator(), fb.vbAlloc, 0, VkDeviceSize(nv) * sizeof(Vertex));
    vmaFlushAllocation(m_ctx->allocator(), fb.ibAlloc, 0, VkDeviceSize(ni) * sizeof(uint16_t));

    // O passe de antes (particulas, fumaca) escreveu na mesma imagem: dependencia de escrita.
    if (v.colorImage)
        m_ctx->cmdImageBarrier(cmd, v.colorImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = v.colorView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = v.depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, v.extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkc::cmdBeginRendering(cmd, &ri);
    const VkViewport viewport{0.0f, 0.0f, float(v.extent.width), float(v.extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, v.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &v.viewProj);
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &fb.vb, &zero);
    vkCmdBindIndexBuffer(cmd, fb.ib, 0, VK_INDEX_TYPE_UINT16);
    VkPipeline bound = VK_NULL_HANDLE;
    int boundTex = -2;
    for (const Batch& b : batches) {
        VkPipeline p = pipelineFor(b.flags);
        if (!p) continue;
        if (p != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            bound = p;
        }
        if (b.tex != boundTex) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_textures[size_t(b.tex)].set, 0,
                                    nullptr);
            boundTex = b.tex;
        }
        vkCmdDrawIndexed(cmd, b.indexCount, 1, b.firstIndex, 0, 0);
        ++m_lastDraws;
    }
    vkc::cmdEndRendering(cmd);
    m_lastVertices = nv;
    return true;
}

} // namespace eruption
