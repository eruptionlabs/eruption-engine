// Animação por esqueleto (glTF skin): ver o comentário de updateSkinning em
// ModelRenderer.hpp. A pose de bind fica no buffer de vértices compartilhado da
// malha; cada instância animada tem o seu buffer, reescrito por
// shaders/compute/skin.comp a cada frame.
#include "renderer/ModelRenderer.hpp"
#include "renderer/GBuffer.hpp"

#include "core/Logger.hpp"
#include "formats/TerrainParser.hpp"
#include "renderer/ShaderCompiler.hpp"

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstring>

namespace eruption {

namespace {

struct SkinVertexGpu {
    uint32_t j01, j23;
    float w[4];
};
static_assert(sizeof(SkinVertexGpu) == 24, "layout do SkinVertex em skin.comp");

struct SkinPush {
    uint32_t vertexCount, jointBase, strideWords, posWord, nrmWord, keepPrev;
};

} // namespace

void ModelRenderer::registerSkinBase(uint32_t mesh, const ModelAsset& asset, const ModelNode& node,
                                     const std::vector<TerrainVertex>& vertices) {
    const SkinRig& rig = *asset.rig;
    if (node.skin < 0 || node.skin >= static_cast<int>(rig.skins.size()) || vertices.empty()) return;
    const SkinRig::Skin& sk = rig.skins[static_cast<size_t>(node.skin)];
    SkinBase b;
    b.rig = asset.rig;
    b.skin = node.skin;
    b.vertexCount = static_cast<uint32_t>(vertices.size());
    std::vector<SkinVertexGpu> data(vertices.size());
    b.jointRadius.assign(sk.joints.size(), 0.0f);
    std::vector<Vec3> bindPos(sk.joints.size());
    for (size_t j = 0; j < sk.joints.size(); ++j) bindPos[j] = Vec3(glm::inverse(sk.inverseBind[j])[3]);
    const uint32_t jointCount = static_cast<uint32_t>(sk.joints.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        SkinVertexGpu& d = data[i];
        d.j01 = node.skinJoints[i * 2];
        d.j23 = node.skinJoints[i * 2 + 1];
        const Vec4 w = node.skinWeights[i];
        const uint32_t js[4] = {d.j01 & 0xFFFFu, d.j01 >> 16, d.j23 & 0xFFFFu, d.j23 >> 16};
        for (int k = 0; k < 4; ++k) {
            d.w[k] = js[k] < jointCount ? w[k] : 0.0f; // osso fora da skin não conta
            if (js[k] >= jointCount) {
                if (k < 2) d.j01 &= k == 0 ? 0xFFFF0000u : 0x0000FFFFu;
                else d.j23 &= k == 2 ? 0xFFFF0000u : 0x0000FFFFu;
                continue;
            }
            if (w[k] > 0.001f)
                b.jointRadius[js[k]] = std::max(b.jointRadius[js[k]], glm::length(vertices[i].position - bindPos[js[k]]));
        }
    }
    const VkDeviceSize bytes = data.size() * sizeof(SkinVertexGpu);
    if (!m_ctx->createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY, b.skinBuffer, b.skinAlloc))
        return;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    m_ctx->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
    void* mapped = nullptr;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, data.data(), bytes);
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);
    const VkBuffer dst = b.skinBuffer;
    m_ctx->immediateSubmit([staging, dst, bytes](VkCommandBuffer cmd) {
        const VkBufferCopy c{0, 0, bytes};
        vkCmdCopyBuffer(cmd, staging, dst, 1, &c);
    });
    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);
    ERUPTION_LOG_WARN("Skin: %s node %s, %u vertices, %zu joints, %zu animations", asset.filePath.c_str(),
                      node.name.c_str(), b.vertexCount, sk.joints.size(), rig.animations.size());
    m_skinBases[mesh] = std::move(b);
}

bool ModelRenderer::ensureSkinGpu() {
    if (m_skinPipeline) return true;
    if (m_skinGpuFailed) return false;
    m_skinGpuFailed = true; // até dar certo
    VkDevice dev = m_ctx->device();
    VkDescriptorSetLayoutBinding binds[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 5;
    li.pBindings = binds;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_skinSetLayout) != VK_SUCCESS) return false;
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPush)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_skinSetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &m_skinPipeLayout) != VK_SUCCESS) return false;
    const std::vector<uint32_t> code = ShaderCompiler::loadSPIRV("compute/skin.comp.spv");
    if (code.empty()) {
        ERUPTION_LOG_ERROR("Skin: compute/skin.comp.spv not found; skinned models stay in bind pose");
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = code.size() * sizeof(uint32_t);
    smi.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &smi, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = "main";
    cpi.layout = m_skinPipeLayout;
    const VkResult r = vkCreateComputePipelines(dev, m_ctx->pipelineCache(), 1, &cpi, nullptr, &m_skinPipeline);
    vkDestroyShaderModule(dev, module, nullptr);
    if (r != VK_SUCCESS) return false;
    // Anel das matrizes dos ossos: uma faixa por frame em voo, mapeada sempre.
    const VkDeviceSize ring = VkDeviceSize(kJointsPerFrame) * VulkanContext::MAX_FRAMES_IN_FLIGHT * sizeof(glm::mat4);
    if (!m_ctx->createBuffer(ring, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, m_jointRing,
                             m_jointRingAlloc))
        return false;
    void* mapped = nullptr;
    vmaMapMemory(m_ctx->allocator(), m_jointRingAlloc, &mapped);
    m_jointMapped = static_cast<glm::mat4*>(mapped);
    m_skinGpuFailed = false;
    return true;
}

void ModelRenderer::makeSkinnedInstance(uint32_t instance) {
    if (instance >= m_instances.size()) return;
    SkinnedInstance s;
    if (!createSkinned(m_instances[instance].meshIndex, s)) return;
    s.instance = instance;
    m_instances[instance].meshIndex = s.mesh;
    m_skinnedByInstance[instance] = m_skinned.size();
    m_skinned.push_back(s);
}

int ModelRenderer::addPreviewSkin(uint32_t baseMesh) {
    SkinnedInstance s;
    if (!createSkinned(baseMesh, s)) return -1;
    s.instance = kNoInstance;
    s.previewId = ++m_previewSkinSeq;
    m_skinned.push_back(s);
    return static_cast<int>(s.previewId);
}

ModelRenderer::SkinnedInstance* ModelRenderer::previewSkin(int id) {
    for (auto& s : m_skinned)
        if (s.instance == kNoInstance && static_cast<int>(s.previewId) == id) return &s;
    return nullptr;
}

uint32_t ModelRenderer::previewSkinMesh(int id) {
    const SkinnedInstance* s = previewSkin(id);
    return s ? s->mesh : 0xFFFFFFFFu;
}

std::vector<std::string> ModelRenderer::previewSkinAnimations(int id) {
    std::vector<std::string> out;
    const SkinnedInstance* s = previewSkin(id);
    if (!s) return out;
    const auto b = m_skinBases.find(s->baseMesh);
    if (b != m_skinBases.end())
        for (const auto& a : b->second.rig->animations) out.push_back(a.name);
    return out;
}

void ModelRenderer::playPreviewSkin(int id, int anim, float speed) {
    SkinnedInstance* s = previewSkin(id);
    if (!s) return;
    if (s->anim != anim) s->time = 0.0f;
    s->anim = anim;
    s->speed = speed;
    s->loop = true;
}

void ModelRenderer::removePreviewSkin(int id) {
    for (size_t k = 0; k < m_skinned.size(); ++k) {
        SkinnedInstance& s = m_skinned[k];
        if (s.instance != kNoInstance || static_cast<int>(s.previewId) != id) continue;
        ModelMeshGPU empty{};
        empty.isExternal = true;
        if (s.mesh < m_meshes.size()) m_meshes[s.mesh] = empty; // índice fica; desenha nada
        releaseSkinned(s);
        m_skinned.erase(m_skinned.begin() + static_cast<std::ptrdiff_t>(k));
        m_skinnedByInstance.clear();
        for (size_t j = 0; j < m_skinned.size(); ++j)
            if (m_skinned[j].instance != kNoInstance) m_skinnedByInstance[m_skinned[j].instance] = j;
        return;
    }
}

bool ModelRenderer::createSkinned(uint32_t base, SkinnedInstance& s) {
    const auto b = m_skinBases.find(base);
    if (b == m_skinBases.end() || !ensureSkinGpu()) return false;
    s.baseMesh = base;
    const VkDeviceSize bytes = VkDeviceSize(b->second.vertexCount) * sizeof(TerrainVertex);
    if (!m_ctx->createBuffer(bytes,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY, s.out, s.outAlloc))
        return false;
    // Com o alvo de velocidade: a pose do frame anterior, no mesmo formato
    // (o vertex shader lê só a posição dela).
    if (GBuffer::velocityEnabled() &&
        !m_ctx->createBuffer(bytes,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_GPU_ONLY, s.prevOut, s.prevOutAlloc)) {
        vmaDestroyBuffer(m_ctx->allocator(), s.out, s.outAlloc);
        return false;
    }
    auto destroyOut = [&] {
        vmaDestroyBuffer(m_ctx->allocator(), s.out, s.outAlloc);
        if (s.prevOut) vmaDestroyBuffer(m_ctx->allocator(), s.prevOut, s.prevOutAlloc);
        s.out = s.prevOut = VK_NULL_HANDLE;
    };
    // Descriptor set (um por instância; pool novo quando o atual enche).
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_skinSetLayout;
    VkResult r = VK_ERROR_OUT_OF_POOL_MEMORY;
    if (!m_skinPools.empty()) {
        ai.descriptorPool = m_skinPools.back();
        r = vkAllocateDescriptorSets(m_ctx->device(), &ai, &s.set);
    }
    if (r != VK_SUCCESS) {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * 256};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 256;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(m_ctx->device(), &pi, nullptr, &pool) != VK_SUCCESS) {
            destroyOut();
            return false;
        }
        m_skinPools.push_back(pool);
        ai.descriptorPool = pool;
        if (vkAllocateDescriptorSets(m_ctx->device(), &ai, &s.set) != VK_SUCCESS) {
            destroyOut();
            return false;
        }
    }
    const VkDescriptorBufferInfo infos[5] = {{m_meshes[base].vertexBuffer, 0, VK_WHOLE_SIZE},
                                             {b->second.skinBuffer, 0, VK_WHOLE_SIZE},
                                             {m_jointRing, 0, VK_WHOLE_SIZE},
                                             {s.out, 0, VK_WHOLE_SIZE},
                                             {s.prevOut ? s.prevOut : s.out, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = s.set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_ctx->device(), 5, w, 0, nullptr);
    // A malha da instância: igual à base, com o buffer de vértices dela e sem
    // os níveis de LOD (que apontariam para a pose de bind).
    ModelMeshGPU clone = m_meshes[base];
    clone.vertexBuffer = s.out;
    clone.prevVertexBuffer = s.prevOut;
    clone.vertexAlloc = VK_NULL_HANDLE;
    clone.hiVertexBuffer = VK_NULL_HANDLE; clone.hiVertexAlloc = VK_NULL_HANDLE;
    clone.hiIndexBuffer = VK_NULL_HANDLE; clone.hiIndexAlloc = VK_NULL_HANDLE;
    clone.hiIndexCount = 0; clone.hiSwitchDistance = 0.0f;
    clone.loIndexBuffer = VK_NULL_HANDLE; clone.loIndexAlloc = VK_NULL_HANDLE;
    clone.loIndexCount = 0; clone.loSwitchDistance = 0.0f;
    clone.farIndexBuffer = VK_NULL_HANDLE; clone.farIndexAlloc = VK_NULL_HANDLE;
    clone.farIndexCount = 0; clone.farSwitchDistance = 0.0f;
    clone.loVertexBuffer = VK_NULL_HANDLE; clone.loVertexAlloc = VK_NULL_HANDLE;
    clone.farVertexBuffer = VK_NULL_HANDLE; clone.farVertexAlloc = VK_NULL_HANDLE;
    clone.shIndexBuffer = VK_NULL_HANDLE; clone.shIndexAlloc = VK_NULL_HANDLE;
    clone.shIndexCount = 0;
    for (auto& md : clone.mid) md = ModelMeshGPU::MidLod{};
    clone.midCount = 0;
    clone.groundCells.clear();
    clone.isExternal = true; // o buffer é do skinning (releaseSkinned)
    m_meshes.push_back(clone);
    s.mesh = static_cast<uint32_t>(m_meshes.size() - 1);
    s.anim = -1; // pose de repouso até a cena ou um script pedir uma animação
    return true;
}

void ModelRenderer::releaseSkinned(SkinnedInstance& s) {
    if (!s.out) return;
    VmaAllocator allocator = m_ctx->allocator();
    VkBuffer buf = s.out, prev = s.prevOut;
    VmaAllocation alloc = s.outAlloc, prevAlloc = s.prevOutAlloc;
    m_ctx->deferFrameCleanup([allocator, buf, alloc, prev, prevAlloc] {
        vmaDestroyBuffer(allocator, buf, alloc);
        if (prev) vmaDestroyBuffer(allocator, prev, prevAlloc);
    });
    s.out = s.prevOut = VK_NULL_HANDLE;
    s.outAlloc = s.prevOutAlloc = VK_NULL_HANDLE;
}

void ModelRenderer::clearSkinning(bool destroyPipeline) {
    VkDevice dev = m_ctx ? m_ctx->device() : VK_NULL_HANDLE;
    if (!dev) return;
    VmaAllocator allocator = m_ctx->allocator();
    std::vector<std::pair<VkBuffer, VmaAllocation>> buffers;
    for (auto& s : m_skinned) {
        if (s.out) buffers.push_back({s.out, s.outAlloc});
        if (s.prevOut) buffers.push_back({s.prevOut, s.prevOutAlloc});
    }
    for (auto& [mesh, b] : m_skinBases)
        if (b.skinBuffer) buffers.push_back({b.skinBuffer, b.skinAlloc});
    std::vector<VkDescriptorPool> pools = std::move(m_skinPools);
    m_skinPools.clear();
    m_skinned.clear();
    m_skinnedByInstance.clear();
    m_skinBases.clear();
    if (destroyPipeline) {
        // Desligando: a GPU já está parada; solta tudo agora.
        for (auto& [buf, alloc] : buffers) vmaDestroyBuffer(allocator, buf, alloc);
        for (VkDescriptorPool p : pools) vkDestroyDescriptorPool(dev, p, nullptr);
        if (m_jointRing) {
            vmaUnmapMemory(allocator, m_jointRingAlloc);
            vmaDestroyBuffer(allocator, m_jointRing, m_jointRingAlloc);
            m_jointRing = VK_NULL_HANDLE;
            m_jointMapped = nullptr;
        }
        if (m_skinPipeline) vkDestroyPipeline(dev, m_skinPipeline, nullptr);
        if (m_skinPipeLayout) vkDestroyPipelineLayout(dev, m_skinPipeLayout, nullptr);
        if (m_skinSetLayout) vkDestroyDescriptorSetLayout(dev, m_skinSetLayout, nullptr);
        m_skinPipeline = VK_NULL_HANDLE;
        m_skinPipeLayout = VK_NULL_HANDLE;
        m_skinSetLayout = VK_NULL_HANDLE;
        return;
    }
    if (buffers.empty() && pools.empty()) return;
    m_ctx->deferFrameCleanup([allocator, dev, buffers, pools] {
        for (auto& [buf, alloc] : buffers) vmaDestroyBuffer(allocator, buf, alloc);
        for (VkDescriptorPool p : pools) vkDestroyDescriptorPool(dev, p, nullptr);
    });
}

std::vector<std::string> ModelRenderer::animationsOf(uint32_t instance) const {
    std::vector<std::string> out;
    const auto it = m_skinnedByInstance.find(instance);
    if (it == m_skinnedByInstance.end()) return out;
    const auto b = m_skinBases.find(m_skinned[it->second].baseMesh);
    if (b == m_skinBases.end()) return out;
    for (const auto& a : b->second.rig->animations) out.push_back(a.name);
    return out;
}

bool ModelRenderer::playAnimation(uint32_t instance, const std::string& name, bool loop, float speed,
                                  float fadeSeconds) {
    const auto it = m_skinnedByInstance.find(instance);
    if (it == m_skinnedByInstance.end()) return false;
    SkinnedInstance& s = m_skinned[it->second];
    const auto b = m_skinBases.find(s.baseMesh);
    if (b == m_skinBases.end()) return false;
    const int a = name.empty() ? -1 : b->second.rig->findAnimation(name);
    if (!name.empty() && a < 0) return false;
    // A pose que está na tela sai aos poucos (se já havia transição, a que
    // chegava vira a que sai: o salto é pequeno com 0,2 s).
    s.fadeSeconds = std::max(fadeSeconds, 0.0f);
    s.fade = 0.0f;
    s.fromAnim = s.anim;
    s.fromTime = s.time;
    s.fromSpeed = s.speed;
    s.fromLoop = s.loop;
    s.anim = a;
    s.time = 0.0f;
    s.loop = loop;
    s.speed = speed;
    return true;
}

std::string ModelRenderer::currentAnimation(uint32_t instance) const {
    const auto it = m_skinnedByInstance.find(instance);
    if (it == m_skinnedByInstance.end()) return {};
    const SkinnedInstance& s = m_skinned[it->second];
    const auto b = m_skinBases.find(s.baseMesh);
    if (b == m_skinBases.end() || s.anim < 0 || s.anim >= static_cast<int>(b->second.rig->animations.size())) return {};
    return b->second.rig->animations[static_cast<size_t>(s.anim)].name;
}

void ModelRenderer::updateSkinning(VkCommandBuffer cmd, float dt) {
    if (m_skinned.empty() || !m_skinPipeline || !m_jointMapped) return;
    const uint32_t frame = m_ctx->currentFrame() % VulkanContext::MAX_FRAMES_IN_FLIGHT;
    glm::mat4* ring = m_jointMapped + size_t(frame) * kJointsPerFrame;
    uint32_t cursor = 0;
    std::vector<glm::mat4> globals;
    struct Job { const SkinnedInstance* s; uint32_t jointBase, vertexCount; };
    std::vector<Job> jobs;
    bool boundsChanged = false;
    for (SkinnedInstance& s : m_skinned) {
        const auto b = m_skinBases.find(s.baseMesh);
        if (b == m_skinBases.end() || !s.out) continue;
        const SkinBase& base = b->second;
        const SkinRig& rig = *base.rig;
        const SkinRig::Skin& sk = rig.skins[static_cast<size_t>(base.skin)];
        const uint32_t joints = static_cast<uint32_t>(sk.joints.size());
        if (joints == 0 || cursor + joints > kJointsPerFrame) continue; // anel cheio: fica parado neste frame
        // Tempo da animação (e da que sai, durante a transição).
        auto advance = [&](int anim, float& time, float speed, bool loop) {
            if (anim < 0 || anim >= static_cast<int>(rig.animations.size())) return;
            const float dur = rig.animations[static_cast<size_t>(anim)].duration;
            time += dt * speed;
            if (dur > 0.0f) {
                if (loop) time = std::fmod(std::max(time, 0.0f), dur);
                else if (time >= dur) time = dur;
            }
        };
        advance(s.anim, s.time, s.speed, s.loop);
        if (s.fade < s.fadeSeconds) {
            advance(s.fromAnim, s.fromTime, s.fromSpeed, s.fromLoop);
            s.fade = std::min(s.fade + dt, s.fadeSeconds);
            const float w = s.fade / s.fadeSeconds;
            // Suave nas pontas: a velocidade dos ossos não dá degrau.
            rig.blendPose(s.fromAnim, s.fromTime, s.anim, s.time, w * w * (3.0f - 2.0f * w), globals);
        } else {
            rig.pose(s.anim, s.time, globals);
        }
        rig.jointMatrices(base.skin, globals, ring + cursor);
        // Limites: cada osso com o alcance dos seus vértices.
        Vec3 lo(FLT_MAX), hi(-FLT_MAX);
        for (uint32_t j = 0; j < joints; ++j) {
            if (base.jointRadius[j] <= 0.0f) continue;
            const int n = sk.joints[j];
            const Vec3 p = n >= 0 && n < static_cast<int>(globals.size()) ? Vec3(globals[static_cast<size_t>(n)][3]) : Vec3(0.0f);
            lo = glm::min(lo, p - Vec3(base.jointRadius[j]));
            hi = glm::max(hi, p + Vec3(base.jointRadius[j]));
        }
        if (lo.x <= hi.x) {
            ModelMeshGPU& mesh = m_meshes[s.mesh];
            mesh.aabbMin = lo;
            mesh.aabbMax = hi;
        }
        if (lo.x <= hi.x && s.instance != kNoInstance && s.instance < m_instances.size()) {
            ModelInstance& inst = m_instances[s.instance];
            Vec3 wMin(FLT_MAX), wMax(-FLT_MAX);
            for (int c = 0; c < 8; ++c) {
                const Vec3 w = Vec3(inst.transform * Vec4((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z, 1.0f));
                wMin = glm::min(wMin, w);
                wMax = glm::max(wMax, w);
            }
            inst.worldAabbMin = wMin;
            inst.worldAabbMax = wMax;
            inst.worldCenter = (wMin + wMax) * 0.5f;
            inst.boundingRadius = glm::length(wMax - wMin) * 0.5f;
            boundsChanged = true;
        }
        jobs.push_back({&s, frame * kJointsPerFrame + cursor, base.vertexCount});
        cursor += joints;
    }
    if (jobs.empty()) return;
    vmaFlushAllocation(m_ctx->allocator(), m_jointRingAlloc, VkDeviceSize(frame) * kJointsPerFrame * sizeof(glm::mat4),
                       VkDeviceSize(cursor) * sizeof(glm::mat4));
    if (boundsChanged) rebuildHotData();

    // Frames anteriores ainda podem estar lendo os vértices (barreira cobre a
    // fila toda); a pose escrita no frame anterior é lida aqui (vira a "prev").
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                           VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0,
                         nullptr);
    // Primeira vez: o vértice inteiro (UV, cor, materiais) vem da base.
    bool copied = false;
    for (const Job& j : jobs) {
        SkinnedInstance& s = *const_cast<SkinnedInstance*>(j.s);
        if (!s.needsInit) continue;
        const VkBufferCopy c{0, 0, VkDeviceSize(j.vertexCount) * sizeof(TerrainVertex)};
        vkCmdCopyBuffer(cmd, m_meshes[s.baseMesh].vertexBuffer, s.out, 1, &c);
        if (s.prevOut) vkCmdCopyBuffer(cmd, m_meshes[s.baseMesh].vertexBuffer, s.prevOut, 1, &c);
        s.needsInit = false;
        copied = true;
    }
    if (copied) {
        VkMemoryBarrier mid{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mid, 0,
                             nullptr, 0, nullptr);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinPipeline);
    for (const Job& j : jobs) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinPipeLayout, 0, 1, &j.s->set, 0, nullptr);
        const SkinPush p{j.vertexCount, j.jointBase, static_cast<uint32_t>(sizeof(TerrainVertex) / 4),
                         static_cast<uint32_t>(offsetof(TerrainVertex, position) / 4),
                         static_cast<uint32_t>(offsetof(TerrainVertex, normal) / 4), j.s->prevOut ? 1u : 0u};
        vkCmdPushConstants(cmd, m_skinPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, (j.vertexCount + 63) / 64, 1, 1);
    }
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &after, 0,
                         nullptr, 0, nullptr);
}

} // namespace eruption
