#include "renderer/ImmediateDraw.hpp"
#include "renderer/MipmapGenerator.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"
#include "core/Project.hpp"
#include "utils/ImageUtils.hpp"
#include "utils/AssetVfs.hpp"
#include "formats/GltfParser.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <glm/gtc/matrix_inverse.hpp>

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

    // Set 2: profundidade da cena para o soft do material. Existe e e' escrito a
    // cada quadro mesmo sem nenhum material (regra do AGENTS.md: todo descritor
    // que o pipeline usa fica valido).
    if (vkCreateDescriptorSetLayout(dev, &dl, nullptr, &m_depthLayout) != VK_SUCCESS) return false;

    constexpr uint32_t kFrames = VulkanContext::MAX_FRAMES_IN_FLIGHT;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures + kFrames},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFrames}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kMaxTextures + 2 * kFrames;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;

    {
        VkDescriptorSetLayout dls[kFrames];
        for (auto& l : dls) l = m_depthLayout;
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = m_pool;
        dai.descriptorSetCount = kFrames;
        dai.pSetLayouts = dls;
        if (vkAllocateDescriptorSets(dev, &dai, m_depthSets) != VK_SUCCESS) return false;
    }

    // Set 3: luz da cena.
    {
        VkDescriptorSetLayoutBinding lb{};
        lb.binding = 0;
        lb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        lb.descriptorCount = 1;
        lb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ll{};
        ll.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ll.bindingCount = 1;
        ll.pBindings = &lb;
        if (vkCreateDescriptorSetLayout(dev, &ll, nullptr, &m_lightLayout) != VK_SUCCESS) return false;
        VkDescriptorSetLayout lls[kFrames];
        for (auto& l : lls) l = m_lightLayout;
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = m_pool;
        dai.descriptorSetCount = kFrames;
        dai.pSetLayouts = lls;
        if (vkAllocateDescriptorSets(dev, &dai, m_lightSets) != VK_SUCCESS) return false;
        for (uint32_t f = 0; f < kFrames; ++f) {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            bi.size = sizeof(Mat4); // 4 x vec4
            bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(ctx->allocator(), &bi, &ai, &m_lightBuf[f], &m_lightAlloc[f], &info) != VK_SUCCESS ||
                !info.pMappedData)
                return false;
            m_lightMap[f] = info.pMappedData;
            std::memset(m_lightMap[f], 0, sizeof(Mat4));
            VkDescriptorBufferInfo db{m_lightBuf[f], 0, sizeof(Mat4)};
            VkWriteDescriptorSet wr{};
            wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr.dstSet = m_lightSets[f];
            wr.descriptorCount = 1;
            wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr.pBufferInfo = &db;
            vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
        }
    }

    // Vertice: viewProj (64 B). Fragmento: material (64 B). Total 128 B, o teto.
    VkPushConstantRange pc[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)},
                                 {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(Mat4), 16 * sizeof(float)}};
    const VkDescriptorSetLayout setLayouts[4] = {m_setLayout, m_setLayout, m_depthLayout, m_lightLayout};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 4;
    li.pSetLayouts = setLayouts;
    li.pushConstantRangeCount = 2;
    li.pPushConstantRanges = pc;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &m_layout) != VK_SUCCESS) return false;

    // REPEAT: o U de um disco "rolando" passa de 1. Linear + mip linear, sem anisotropia.
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(dev, &si, nullptr, &m_sampler) != VK_SUCCESS) return false;
    // Profundidade: vizinho mais proximo (D32 nao aceita linear em PowerVR) e sem repeticao.
    VkSamplerCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ds.magFilter = ds.minFilter = VK_FILTER_NEAREST;
    ds.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ds.addressModeU = ds.addressModeV = ds.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &ds, nullptr, &m_depthSampler) != VK_SUCCESS) return false;

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
    if (m_depthSampler) vkDestroySampler(dev, m_depthSampler, nullptr);
    for (uint32_t f = 0; f < VulkanContext::MAX_FRAMES_IN_FLIGHT; ++f) {
        if (m_lightBuf[f]) vmaDestroyBuffer(m_ctx->allocator(), m_lightBuf[f], m_lightAlloc[f]);
        m_lightBuf[f] = VK_NULL_HANDLE;
        m_lightMap[f] = nullptr;
    }
    if (m_lightLayout) vkDestroyDescriptorSetLayout(dev, m_lightLayout, nullptr);
    m_lightLayout = VK_NULL_HANDLE;
    if (m_depthLayout) vkDestroyDescriptorSetLayout(dev, m_depthLayout, nullptr);
    m_depthSampler = VK_NULL_HANDLE;
    m_depthLayout = VK_NULL_HANDLE;
    for (auto& v : m_depthSetView) v = VK_NULL_HANDLE;
    m_meshData.clear();
    m_meshIds.clear();
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

uint32_t ImmediateDraw::materialIndex(const Material* mat) {
    if (!mat || !mat->any()) return 0;
    for (size_t i = 0; i < m_mats.size(); ++i)
        if (m_mats[i] == *mat) return uint32_t(i) + 1;
    m_mats.push_back(*mat);
    return uint32_t(m_mats.size());
}

ImmediateDraw::Vertex* ImmediateDraw::addGeometry(int tex, Topology topo, Flags flags, uint32_t count,
                                                  const Material* mat) {
    if (!m_ready || tex < 0) return nullptr;
    if (topo == Topology::Quads) count &= ~3u;
    if (count < (topo == Topology::Quads ? 4u : 3u)) return nullptr;
    if (m_pending + count > kMaxVertices) {
        warnOnce("quadro cheio");
        return nullptr;
    }
    const uint32_t first = uint32_t(m_verts.size());
    m_verts.resize(m_verts.size() + count);
    m_cmds.push_back({tex, packFlags(flags), uint8_t(topo == Topology::Quads ? 1 : 0), first, count, materialIndex(mat), 0, 0});
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
    m_cmds.push_back({tex, packFlags(flags), 2, first, 4, 0, 0, 0});
    m_pending += 4;
}

// Primeira malha com triangulos do .glb (como as malhas de emissor do VFX: nos
// sem hierarquia, cor de vertice ignorada).
bool ImmediateDraw::loadMesh(const std::string& path, MeshData& out) {
    std::filesystem::path p = project::gameRef(path);
    if (!vfs::exists(p)) p = project::engine(path);
    std::vector<uint8_t> bytes;
    if (!vfs::read(p.string(), bytes)) return false;
    ModelFile mf;
    try {
        mf = GltfParser::parse(bytes, p.parent_path().string());
    } catch (...) {
        return false;
    }
    for (const ModelFileNode& n : mf.nodes) {
        if (n.vertices.empty() || n.indices.size() < 3) continue;
        // Cor base do material (a 1a textura do no): a imagem do .glb ou o pixel solido
        // que o parser sintetiza do baseColorFactor.
        if (!n.textureNames.empty()) {
            for (const EmbeddedTexture& et : mf.embeddedTextures) {
                if (et.name != n.textureNames[0]) continue;
                int w = et.width, h = et.height;
                std::vector<uint8_t> px;
                if (et.isDecoded() && et.channels == 4) {
                    px = et.pixels;
                } else if (et.hasEncodedData()) {
                    ImageData img = ImageUtils::loadFromMemory(et.encodedData.data(), et.encodedData.size());
                    if (img.isValid() && img.channels == 4) {
                        w = img.width;
                        h = img.height;
                        px = std::move(img.pixels);
                    }
                }
                if (!px.empty() && w > 0 && h > 0) {
                    ImageUtils::downscaleRGBA(w, h, px, 512);
                    out.baseRgba = std::move(px);
                    out.baseW = w;
                    out.baseH = h;
                }
                break;
            }
        }
        if (n.vertices.size() > kMaxMeshVertices) {
            ERUPTION_LOG_WARN("ImmediateDraw: malha '%s' com %zu vertices (maximo %u)", path.c_str(), n.vertices.size(),
                              kMaxMeshVertices);
            return false;
        }
        out.verts.resize(n.vertices.size());
        for (size_t i = 0; i < n.vertices.size(); ++i) {
            Vertex& v = out.verts[i];
            v = Vertex{};
            v.x = n.vertices[i].x; v.y = n.vertices[i].y; v.z = n.vertices[i].z;
            if (i < n.texCoords.size()) { v.u = n.texCoords[i].x; v.v = n.texCoords[i].y; }
            if (i < n.normals.size()) { v.nx = n.normals[i].x; v.ny = n.normals[i].y; v.nz = n.normals[i].z; }
            v.r = v.g = v.b = v.a = 1.0f;
        }
        const size_t tris = n.indices.size() / 3;
        for (size_t i = 0; i < tris * 3; ++i) {
            if (n.indices[i] >= n.vertices.size()) {
                out.verts.clear();
                out.idx.clear();
                return false;
            }
            out.idx.push_back(uint16_t(n.indices[i]));
        }
        return true;
    }
    return false;
}

int ImmediateDraw::meshId(const std::string& path) {
    if (!m_ready) return -1;
    auto it = m_meshIds.find(path);
    if (it != m_meshIds.end()) return it->second;
    int id = -1;
    MeshData md;
    if (m_meshData.size() >= kMaxMeshes) {
        ERUPTION_LOG_WARN("ImmediateDraw: mais de %u malhas diferentes, '%s' ignorada", kMaxMeshes, path.c_str());
    } else if (loadMesh(path, md)) {
        id = int(m_meshData.size());
        md.path = path;
        m_meshData.push_back(std::move(md));
    } else {
        ERUPTION_LOG_WARN("ImmediateDraw: malha '%s' nao abriu", path.c_str());
    }
    m_meshIds.emplace(path, id); // a falha tambem fica em cache
    return id;
}

void ImmediateDraw::addMesh(int mesh, int tex, const Mat4& xf, const Vec4& tint, Flags flags, const Material* mat) {
    if (!m_ready || mesh < 0 || size_t(mesh) >= m_meshData.size()) return;
    MeshData& md = m_meshData[size_t(mesh)];
    // Com luz e sem textura pedida, a cor base do proprio .glb (sobe uma vez por malha).
    if (tex < 0 && mat && mat->lit) {
        if (md.baseTex == -2) {
            md.baseTex = md.baseRgba.empty() ? -1 : addTexture(md.path + "#base", md.baseRgba.data(), md.baseW, md.baseH);
            std::vector<uint8_t>().swap(md.baseRgba);
        }
        tex = md.baseTex;
    }
    if (tex < 0) {
        static const uint8_t kWhite[4] = {255, 255, 255, 255};
        tex = addTexture("__white", kWhite, 1, 1);
        if (tex < 0) return;
    }
    const uint32_t count = uint32_t(md.verts.size());
    if (m_pending + count > kMaxVertices) {
        warnOnce("quadro cheio");
        return;
    }
    const Mat3 nm = glm::transpose(glm::inverse(Mat3(xf)));
    const uint32_t first = uint32_t(m_verts.size());
    m_verts.resize(m_verts.size() + count);
    for (uint32_t i = 0; i < count; ++i) {
        const Vertex& s = md.verts[i];
        Vertex& d = m_verts[first + i];
        const Vec3 p = Vec3(xf * Vec4(s.x, s.y, s.z, 1.0f));
        Vec3 n = nm * Vec3(s.nx, s.ny, s.nz);
        const float nl = glm::length(n);
        n = nl > 1e-6f ? n / nl : Vec3(0.0f);
        d = {p.x, p.y, p.z, s.u, s.v, tint.r, tint.g, tint.b, tint.a, n.x, n.y, n.z};
    }
    const uint32_t idxFirst = uint32_t(m_idx.size());
    m_idx.insert(m_idx.end(), md.idx.begin(), md.idx.end());
    m_cmds.push_back({tex, packFlags(flags), 3, first, count, materialIndex(mat), idxFirst, uint32_t(md.idx.size())});
    m_pending += count;
}

void ImmediateDraw::writeDepthSet(uint32_t frame, VkImageView depthView) {
    if (m_depthSetView[frame] == depthView) return;
    VkDescriptorImageInfo ii{m_depthSampler, depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = m_depthSets[frame];
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &wr, 0, nullptr);
    m_depthSetView[frame] = depthView;
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
    VkVertexInputAttributeDescription attrs[4] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r)},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)},
    };
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &binding;
    vin.vertexAttributeDescriptionCount = 4;
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
    struct Batch { int tex; uint8_t flags; uint32_t mat, firstIndex, indexCount; };
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
            if (ca.flags != cb.flags) return ca.flags < cb.flags;
            return ca.mat < cb.mat;
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
        if (ni + (c.kind == 3 ? c.idxCount : (c.kind == 2 ? 6u : c.count * 3u)) > kMaxIndices) continue;
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
            if (c.kind == 3) {
                for (uint32_t k = 0; k < c.idxCount; ++k) iout[ni++] = uint16_t(base + m_idx[c.idxFirst + k]);
            } else if (c.kind == 1) {
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
            batches.back().mat == c.mat && batches.back().firstIndex + batches.back().indexCount == idxStart)
            batches.back().indexCount += ni - idxStart;
        else
            batches.push_back({c.tex, c.flags, c.mat, idxStart, ni - idxStart});
    }
    m_cmds.clear();
    m_verts.clear();
    m_sprites.clear();
    m_idx.clear();
    m_pending = 0;
    if (batches.empty() || ni == 0) {
        m_mats.clear();
        return false;
    }
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
    const uint32_t frameIdx = m_ctx->currentFrame() % VulkanContext::MAX_FRAMES_IN_FLIGHT;
    writeDepthSet(frameIdx, v.depthView);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 2, 1, &m_depthSets[frameIdx], 0, nullptr);
    {
        // Luz da cena (so' le' quem pediu Material::lit; sempre valida).
        const Vec4 ld[4] = {v.light.sunDir, v.light.sunColor, v.light.ambientSky, v.light.ambientGround};
        std::memcpy(m_lightMap[frameIdx], ld, sizeof(ld));
        vmaFlushAllocation(m_ctx->allocator(), m_lightAlloc[frameIdx], 0, sizeof(ld));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 3, 1, &m_lightSets[frameIdx], 0, nullptr);
    }
    // Material: flags = 0 (o desenho de sempre) ate' um lote pedir outro.
    auto pushMaterial = [&](uint32_t id) {
        float f[16] = {};
        if (id != 0) {
            const Material& m = m_mats[id - 1];
            const bool noise = m.noiseTex >= 0;
            const double t = v.time;
            f[0] = m.fresnelPower;
            f[1] = m.invert ? 1.0f : 0.0f;
            f[2] = m.softDepth;
            f[3] = float((m.fresnel ? 1 : 0) | (noise ? 2 : 0) | (m.softDepth > 0.0f ? 4 : 0) | (m.lit ? 8 : 0));
            f[4] = float(std::fmod(double(m.scrollU) * t, 1.0));
            f[5] = float(std::fmod(double(m.scrollV) * t, 1.0));
            f[6] = m.tileU;
            f[7] = m.tileV;
            f[8] = m.erode;
            f[9] = m.erodeSoft;
            f[10] = v.depthProj.x;
            f[11] = v.depthProj.y;
            f[12] = v.depthProj.z;
            f[13] = v.camPos.x;
            f[14] = v.camPos.y;
            f[15] = v.camPos.z;
        }
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(Mat4), sizeof(f), f);
    };
    pushMaterial(0);
    uint32_t boundMat = 0;
    int boundNoise = -2;
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
        if (b.mat != boundMat) {
            pushMaterial(b.mat);
            boundMat = b.mat;
        }
        // Set 1 (ruido) sempre valido: sem ruido, a propria textura do lote.
        const int noiseTex = b.mat != 0 && m_mats[b.mat - 1].noiseTex >= 0 &&
                                     size_t(m_mats[b.mat - 1].noiseTex) < m_textures.size()
                                 ? m_mats[b.mat - 1].noiseTex
                                 : b.tex;
        if (noiseTex != boundNoise) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &m_textures[size_t(noiseTex)].set,
                                    0, nullptr);
            boundNoise = noiseTex;
        }
        vkCmdDrawIndexed(cmd, b.indexCount, 1, b.firstIndex, 0, 0);
        ++m_lastDraws;
    }
    vkc::cmdEndRendering(cmd);
    m_mats.clear();
    m_lastVertices = nv;
    return true;
}

} // namespace eruption
