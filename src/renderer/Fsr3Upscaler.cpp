#include "renderer/Fsr3Upscaler.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/Logger.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

// Host do FSR 3.1 escrito a partir do codigo de referencia do AMD FidelityFX
// SDK v1.1.4 (ffx_fsr3upscaler.cpp, MIT). Numeros entre parenteses nos
// comentarios ("cpp:N") apontam para aquele arquivo, nao para este.

namespace eruption {

namespace {

constexpr uint32_t kSamplerPointBinding = 1000;  // s_PointClamp
constexpr uint32_t kSamplerLinearBinding = 1001; // s_LinearClamp
constexpr VkDescriptorType kSampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
constexpr VkDescriptorType kStorage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType kUbo = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;

struct SpdConstants {
    uint32_t mips;
    uint32_t numWorkGroups;
    uint32_t workGroupOffset[2];
    uint32_t renderSize[2];
};
struct RcasConstants {
    uint32_t rcasConfig[4];
};

const char* kPassFile[Fsr3Upscaler::PassCount] = {
    "prepare_inputs", "luma_pyramid", "shading_change_pyramid", "shading_change",
    "prepare_reactivity", "luma_instability", "accumulate", "rcas", "debug_view"};

uint32_t divUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = srcStage;
    mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage;
    mb.dstAccessMask = dstAccess;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// Entre passes: tudo em GENERAL, uma barreira de memoria compute -> compute
// basta (o backend Vulkan do SDK faz o equivalente por recurso).
void computeToCompute(VkCommandBuffer cmd) {
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

void clearColor(VkCommandBuffer cmd, VkImage image, const VkClearColorValue& value, uint32_t mips = 1) {
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
}

VkClearColorValue clearFloat(float r, float g = 0.0f, float b = 0.0f, float a = 0.0f) {
    VkClearColorValue v{};
    v.float32[0] = r; v.float32[1] = g; v.float32[2] = b; v.float32[3] = a;
    return v;
}

VkClearColorValue clearUint(uint32_t r) {
    VkClearColorValue v{};
    v.uint32[0] = r;
    return v;
}

} // namespace

uint32_t Fsr3Upscaler::jitterPhaseCount(uint32_t renderW, uint32_t displayW) {
    const float ratio = float(std::max(1u, displayW)) / float(std::max(1u, renderW));
    return std::max(1u, static_cast<uint32_t>(8.0f * ratio * ratio)); // cpp:1319-1324
}

// ---------------------------------------------------------------------------
// Criacao

bool Fsr3Upscaler::init(VulkanContext* ctx, bool hdr, uint32_t renderW, uint32_t renderH,
                        uint32_t displayW, uint32_t displayH) {
    m_ctx = ctx;
    m_hdr = hdr;
    m_renderW = renderW; m_renderH = renderH;
    m_displayW = displayW; m_displayH = displayH;
    if (!supported(ctx)) {
        Logger::warning("FSR: device sem os recursos exigidos (subgroup quad em compute / escrita sem formato)");
        return false;
    }
    if (!createStatic() || !createSizeDependent()) {
        shutdown();
        return false;
    }
    return true;
}

void Fsr3Upscaler::shutdown() {
    if (!m_ctx) return;
    vkDeviceWaitIdle(m_ctx->device());
    destroySizeDependent();
    destroyStatic();
    m_ctx = nullptr;
}

void Fsr3Upscaler::resize(uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH) {
    if (!m_ctx) return;
    if (renderW == m_renderW && renderH == m_renderH && displayW == m_displayW && displayH == m_displayH) return;
    vkDeviceWaitIdle(m_ctx->device());
    destroySizeDependent();
    m_renderW = renderW; m_renderH = renderH;
    m_displayW = displayW; m_displayH = displayH;
    createSizeDependent();
}

bool Fsr3Upscaler::createStatic() {
    VkDevice dev = m_ctx->device();

    // Os dois samplers imutaveis que todo passe declara (cb:283-284, vk:3261-3300).
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0f;
    si.maxLod = VK_LOD_CLAMP_NONE;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(dev, &si, nullptr, &m_pointClamp) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (vkCreateSampler(dev, &si, nullptr, &m_linearClamp) != VK_SUCCESS) return false;

    // Tabela de bindings por passe (os FSR3UPSCALER_BIND_* de cada *_pass.glsl).
    using R = Res;
    m_passes[PrepareInputs].bindings = {
        {0, kSampled, R::InMotion}, {1, kSampled, R::InDepth}, {2, kSampled, R::InColor},
        {3, kStorage, R::DilatedMotion}, {4, kStorage, R::DilatedDepth}, {5, kStorage, R::ReconPrevDepth},
        {6, kStorage, R::Intermediate16}, {7, kStorage, R::CurrentLuma}, {8, kUbo, R::Cb0}};
    m_passes[LumaPyramid].bindings = {
        {0, kSampled, R::CurrentLuma}, {1, kSampled, R::Intermediate16}, {2, kStorage, R::SpdAtomic},
        {3, kStorage, R::FrameInfo}, {4, kStorage, R::SpdMip0}, {5, kStorage, R::SpdMip1},
        {6, kStorage, R::SpdMip2}, {7, kStorage, R::SpdMip3}, {8, kStorage, R::SpdMip4},
        {9, kStorage, R::SpdMip5}, {10, kStorage, R::FarthestMip1}, {11, kUbo, R::Cb0}, {12, kUbo, R::CbSpd}};
    m_passes[ShadingChangePyramid].bindings = {
        {0, kSampled, R::CurrentLuma}, {1, kSampled, R::PreviousLuma}, {2, kSampled, R::DilatedMotion},
        {3, kSampled, R::InExposure}, {4, kStorage, R::SpdAtomic}, {5, kStorage, R::SpdMip0},
        {6, kStorage, R::SpdMip1}, {7, kStorage, R::SpdMip2}, {8, kStorage, R::SpdMip3},
        {9, kStorage, R::SpdMip4}, {10, kStorage, R::SpdMip5}, {11, kUbo, R::Cb0}, {12, kUbo, R::CbSpd}};
    m_passes[ShadingChange].bindings = {
        {0, kSampled, R::SpdMipsAll}, {1, kStorage, R::ShadingChangeImg}, {2, kUbo, R::Cb0}};
    m_passes[PrepareReactivity].bindings = {
        {0, kSampled, R::ReconPrevDepth}, {1, kSampled, R::DilatedMotion}, {2, kSampled, R::DilatedDepth},
        {3, kSampled, R::InReactive}, {4, kSampled, R::InTransparency}, {5, kSampled, R::AccumSrv},
        {6, kSampled, R::ShadingChangeImg}, {7, kSampled, R::CurrentLuma}, {8, kSampled, R::InExposure},
        {9, kStorage, R::DilatedReactive}, {10, kStorage, R::NewLocks}, {11, kStorage, R::AccumUav},
        {12, kUbo, R::Cb0}};
    m_passes[LumaInstability].bindings = {
        {0, kSampled, R::InExposure}, {1, kSampled, R::DilatedReactive}, {2, kSampled, R::DilatedMotion},
        {3, kSampled, R::FrameInfo}, {4, kSampled, R::LumaHistSrv}, {5, kSampled, R::FarthestMip1},
        {6, kSampled, R::CurrentLuma}, {7, kStorage, R::LumaHistUav}, {8, kStorage, R::Intermediate16},
        {9, kUbo, R::Cb0}};
    m_passes[Accumulate].bindings = {
        {0, kSampled, R::InExposure}, {1, kSampled, R::DilatedReactive}, {2, kSampled, R::DilatedMotion},
        {3, kSampled, R::UpscaledSrv}, {4, kSampled, R::LanczosLut}, {5, kSampled, R::FarthestMip1},
        {6, kSampled, R::CurrentLuma}, {7, kSampled, R::Intermediate16}, {8, kSampled, R::InColor},
        {9, kStorage, R::UpscaledUav}, {10, kStorage, R::Output}, {11, kStorage, R::NewLocks},
        {12, kUbo, R::Cb0}};
    m_passes[Rcas].bindings = {
        {0, kSampled, R::InExposure}, {1, kSampled, R::UpscaledUav}, {2, kStorage, R::Output},
        {3, kUbo, R::Cb0}, {4, kUbo, R::CbRcas}};
    m_passes[DebugView].bindings = {
        {0, kSampled, R::DilatedReactive}, {1, kSampled, R::DilatedMotion}, {2, kSampled, R::DilatedDepth},
        {3, kSampled, R::UpscaledSrv}, {4, kSampled, R::InExposure}, {5, kStorage, R::Output},
        {6, kUbo, R::Cb0}};

    const VkSampler immutable[2] = {m_pointClamp, m_linearClamp};
    uint32_t nSampled = 0, nStorage = 0, nUbo = 0;
    const std::string suffix = m_hdr ? "_hdr.comp.spv" : "_ldr.comp.spv";
    auto makePipeline = [&](const std::string& file, VkPipelineLayout layout) -> VkPipeline {
        const auto code = ShaderCompiler::loadSPIRV("fsr3/" + file + suffix);
        if (code.empty()) {
            Logger::error("FSR: shaders/fsr3/%s%s nao encontrado", file.c_str(), suffix.c_str());
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = code.size() * sizeof(uint32_t);
        smi.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(dev, &smi, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = layout;
        VkPipeline p = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(dev, m_ctx->pipelineCache(), 1, &ci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
        vkDestroyShaderModule(dev, module, nullptr);
        if (p == VK_NULL_HANDLE) Logger::error("FSR: falha ao criar a pipeline %s", file.c_str());
        return p;
    };

    for (uint32_t i = 0; i < PassCount; ++i) {
        PassObjects& pass = m_passes[i];
        std::vector<VkDescriptorSetLayoutBinding> lb;
        for (const Binding& b : pass.bindings) {
            VkDescriptorSetLayoutBinding x{};
            x.binding = b.binding;
            x.descriptorType = b.type;
            x.descriptorCount = 1;
            x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            lb.push_back(x);
            if (b.type == kSampled) ++nSampled;
            else if (b.type == kStorage) ++nStorage;
            else ++nUbo;
        }
        for (uint32_t s = 0; s < 2; ++s) {
            VkDescriptorSetLayoutBinding x{};
            x.binding = s == 0 ? kSamplerPointBinding : kSamplerLinearBinding;
            x.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            x.descriptorCount = 1;
            x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            x.pImmutableSamplers = &immutable[s];
            lb.push_back(x);
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = static_cast<uint32_t>(lb.size());
        li.pBindings = lb.data();
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &pass.setLayout) != VK_SUCCESS) return false;
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &pass.setLayout;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &pass.layout) != VK_SUCCESS) return false;
        pass.pipeline = makePipeline(kPassFile[i], pass.layout);
        if (pass.pipeline == VK_NULL_HANDLE) return false;
        if (i == Accumulate) {
            pass.pipelineAlt = makePipeline("accumulate_sharpen", pass.layout);
            if (pass.pipelineAlt == VK_NULL_HANDLE) return false;
        }
    }

    // Dois sets por passe (paridade do ping-pong de historico).
    const VkDescriptorPoolSize ps[4] = {
        {kSampled, nSampled * 2}, {kStorage, nStorage * 2}, {kUbo, nUbo * 2},
        {VK_DESCRIPTOR_TYPE_SAMPLER, PassCount * 2 * 2}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = PassCount * 2;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    for (PassObjects& pass : m_passes) {
        const VkDescriptorSetLayout layouts[2] = {pass.setLayout, pass.setLayout};
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(dev, &ai, pass.sets.data()) != VK_SUCCESS) return false;
    }

    // UBO dinamico persistente: uma fatia [cb0 | spd | rcas] por frame em voo.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_ctx->physicalDevice(), &props);
    const VkDeviceSize align = std::max<VkDeviceSize>(1, props.limits.minUniformBufferOffsetAlignment);
    m_uboStride = ((256 + align - 1) / align) * align;
    const VkDeviceSize uboSize = m_uboStride * 3 * VulkanContext::MAX_FRAMES_IN_FLIGHT;
    if (!m_ctx->createBuffer(uboSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
                             m_ubo, m_uboAlloc)) return false;
    void* mapped = nullptr;
    if (vmaMapMemory(m_ctx->allocator(), m_uboAlloc, &mapped) != VK_SUCCESS) return false;
    m_uboMapped = static_cast<uint8_t*>(mapped);
    std::memset(m_uboMapped, 0, size_t(uboSize));
    return true;
}

void Fsr3Upscaler::destroyStatic() {
    VkDevice dev = m_ctx->device();
    if (m_uboMapped) { vmaUnmapMemory(m_ctx->allocator(), m_uboAlloc); m_uboMapped = nullptr; }
    if (m_ubo) { vmaDestroyBuffer(m_ctx->allocator(), m_ubo, m_uboAlloc); m_ubo = VK_NULL_HANDLE; }
    for (PassObjects& pass : m_passes) {
        if (pass.pipeline) vkDestroyPipeline(dev, pass.pipeline, nullptr);
        if (pass.pipelineAlt) vkDestroyPipeline(dev, pass.pipelineAlt, nullptr);
        if (pass.layout) vkDestroyPipelineLayout(dev, pass.layout, nullptr);
        if (pass.setLayout) vkDestroyDescriptorSetLayout(dev, pass.setLayout, nullptr);
        pass = PassObjects{};
    }
    if (m_pool) { vkDestroyDescriptorPool(dev, m_pool, nullptr); m_pool = VK_NULL_HANDLE; }
    if (m_pointClamp) { vkDestroySampler(dev, m_pointClamp, nullptr); m_pointClamp = VK_NULL_HANDLE; }
    if (m_linearClamp) { vkDestroySampler(dev, m_linearClamp, nullptr); m_linearClamp = VK_NULL_HANDLE; }
}

bool Fsr3Upscaler::createTex(Tex& t, uint32_t w, uint32_t h, VkFormat fmt, uint32_t mips) {
    t.width = std::max(1u, w);
    t.height = std::max(1u, h);
    t.format = fmt;
    t.mips = mips;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fmt != VK_FORMAT_R16_SNORM && fmt != VK_FORMAT_R32G32_SFLOAT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    // A saida tambem e' alvo de cor: a camada de sprites desenha por cima dela.
    if (&t == &m_output) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!m_ctx->createImage(t.width, t.height, fmt, usage, VMA_MEMORY_USAGE_GPU_ONLY, t.image, t.alloc, mips)) return false;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    if (vkCreateImageView(m_ctx->device(), &vi, nullptr, &t.view) != VK_SUCCESS) return false;
    if (mips > 1) {
        for (uint32_t m = 0; m < 6 && m < mips; ++m) {
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
            if (vkCreateImageView(m_ctx->device(), &vi, nullptr, &t.mipViews[m]) != VK_SUCCESS) return false;
        }
    }
    return true;
}

void Fsr3Upscaler::destroyTex(Tex& t) {
    VkDevice dev = m_ctx->device();
    for (VkImageView& v : t.mipViews) {
        if (v) vkDestroyImageView(dev, v, nullptr);
        v = VK_NULL_HANDLE;
    }
    if (t.view) vkDestroyImageView(dev, t.view, nullptr);
    if (t.image) vmaDestroyImage(m_ctx->allocator(), t.image, t.alloc);
    t = Tex{};
}

bool Fsr3Upscaler::createSizeDependent() {
    // Tamanhos EXATOS: os shaders enderecam as imagens internas dividindo por
    // maxRenderSize/maxUpscaleSize das constantes (common.h:268-275).
    const uint32_t rw = m_renderW, rh = m_renderH, dw = m_displayW, dh = m_displayH;
    const uint32_t hw = std::max(1u, rw / 2), hh = std::max(1u, rh / 2);
    uint32_t spdMips = 1;
    for (uint32_t s = std::max(hw, hh); s > 1; s >>= 1) ++spdMips;
    spdMips = std::max(spdMips, 6u);
    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        ok &= createTex(m_accum[i], rw, rh, VK_FORMAT_R8_UNORM);
        ok &= createTex(m_luma[i], rw, rh, VK_FORMAT_R16_SFLOAT);
        ok &= createTex(m_lumaHist[i], rw, rh, VK_FORMAT_R16G16B16A16_SFLOAT);
        ok &= createTex(m_upscaled[i], dw, dh, VK_FORMAT_R16G16B16A16_SFLOAT);
    }
    ok &= createTex(m_intermediate16, rw, rh, VK_FORMAT_R16_SFLOAT);
    ok &= createTex(m_shadingChange, hw, hh, VK_FORMAT_R8_UNORM);
    ok &= createTex(m_newLocks, dw, dh, VK_FORMAT_R8_UNORM);
    ok &= createTex(m_spdMips, hw, hh, VK_FORMAT_R16G16_SFLOAT, spdMips);
    ok &= createTex(m_farthestMip1, hw, hh, VK_FORMAT_R16_SFLOAT);
    ok &= createTex(m_spdAtomic, 1, 1, VK_FORMAT_R32_UINT);
    ok &= createTex(m_dilatedReactive, rw, rh, VK_FORMAT_R8G8B8A8_UNORM);
    ok &= createTex(m_lanczosLut, 128, 1, VK_FORMAT_R16_SNORM);
    ok &= createTex(m_defaultReactive, 1, 1, VK_FORMAT_R8_UNORM);
    ok &= createTex(m_defaultExposure, 1, 1, VK_FORMAT_R32G32_SFLOAT);
    ok &= createTex(m_frameInfo, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT);
    ok &= createTex(m_dilatedDepth, rw, rh, VK_FORMAT_R32_SFLOAT);
    ok &= createTex(m_dilatedMotion, rw, rh, VK_FORMAT_R16G16_SFLOAT);
    ok &= createTex(m_reconPrevDepth, rw, rh, VK_FORMAT_R32_UINT);
    ok &= createTex(m_output, dw, dh, kOutputFormat);
    if (!ok) {
        Logger::error("FSR: falha ao criar as imagens internas (%ux%u -> %ux%u)", rw, rh, dw, dh);
        return false;
    }
    initialUploadsAndClears();

    // Estado do contexto (cpp:527-537).
    m_c = Constants{};
    m_c.maxUpscaleSize[0] = int32_t(dw);
    m_c.maxUpscaleSize[1] = int32_t(dh);
    m_c.velocityFactor = 1.0f;
    m_c.reactivenessScale = 1.0f;
    m_c.shadingChangeScale = 1.0f;
    m_c.accumulationAddedPerFrame = 1.0f / 3.0f;
    m_c.minDisocclusionAccumulation = -1.0f / 3.0f;
    m_firstExecution = true;
    m_resourceFrameIndex = 0;
    m_preExposure = m_prevPreExposure = 0.0f;
    writeSets();
    return true;
}

void Fsr3Upscaler::destroySizeDependent() {
    for (int i = 0; i < 2; ++i) {
        destroyTex(m_accum[i]); destroyTex(m_luma[i]); destroyTex(m_lumaHist[i]); destroyTex(m_upscaled[i]);
    }
    Tex* single[] = {&m_intermediate16, &m_shadingChange, &m_newLocks, &m_spdMips, &m_farthestMip1,
                     &m_spdAtomic, &m_dilatedReactive, &m_lanczosLut, &m_defaultReactive,
                     &m_defaultExposure, &m_frameInfo, &m_dilatedDepth, &m_dilatedMotion,
                     &m_reconPrevDepth, &m_output};
    for (Tex* t : single) destroyTex(*t);
}

void Fsr3Upscaler::initialUploadsAndClears() {
    // LUT de Lanczos 2 (cpp:540-548). Com REPROJECT_USE_LANCZOS_TYPE=0 nada a
    // le', mas o accumulate declara o binding: a imagem existe com o conteudo certo.
    int16_t lut[128];
    for (int i = 0; i < 128; ++i) {
        const float x = 2.0f * float(i) / 127.0f;
        float y = 1.0f;
        if (std::fabs(x) > 1e-6f) {
            const float px = 3.14159265358979f * x;
            y = (std::sin(px) / px) * (std::sin(px * 0.5f) / (px * 0.5f));
        }
        lut[i] = static_cast<int16_t>(std::lround(y * 32767.0f));
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    m_ctx->createBuffer(sizeof(lut), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
    void* p = nullptr;
    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &p);
    std::memcpy(p, lut, sizeof(lut));
    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

    std::vector<Tex*> all = {&m_intermediate16, &m_shadingChange, &m_newLocks, &m_spdMips, &m_farthestMip1,
                             &m_spdAtomic, &m_dilatedReactive, &m_lanczosLut, &m_defaultReactive,
                             &m_defaultExposure, &m_frameInfo, &m_dilatedDepth, &m_dilatedMotion,
                             &m_reconPrevDepth, &m_output};
    for (int i = 0; i < 2; ++i) {
        all.push_back(&m_accum[i]); all.push_back(&m_luma[i]);
        all.push_back(&m_lumaHist[i]); all.push_back(&m_upscaled[i]);
    }
    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
        std::vector<VkImageMemoryBarrier2> toGeneral;
        for (Tex* t : all) {
            VkImageMemoryBarrier2 b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = t->image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->mips, 0, 1};
            toGeneral.push_back(b);
        }
        m_ctx->cmdImageBarriers(cmd, toGeneral);

        // Tudo comeca em zero: o SDK deixa NEW_LOCKS e o historico sem
        // inicializar (G4), e lixo ali vira trava de pixel no frame 0.
        for (Tex* t : all) {
            if (t == &m_lanczosLut) continue;
            const bool isUint = t->format == VK_FORMAT_R32_UINT;
            clearColor(cmd, t->image, isUint ? clearUint(0) : clearFloat(0.0f), t->mips);
        }
        clearColor(cmd, m_frameInfo.image, clearFloat(-1.0f, 1.0f, 0.0f, 0.0f));

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {128, 1, 1};
        vkCmdCopyBufferToImage(cmd, staging, m_lanczosLut.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);

        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    });
    vmaDestroyBuffer(m_ctx->allocator(), staging, stagingAlloc);
    m_outputLayout = VK_IMAGE_LAYOUT_GENERAL;
}

// ---------------------------------------------------------------------------
// Descritores

void Fsr3Upscaler::bindInputs(VkImageView color, VkImageView depth, VkImageView motion, VkImageView reactive) {
    m_inReactive = reactive;
    m_inColor = color;
    m_inDepth = depth;
    m_inMotion = motion;
    writeSets();
}

VkImageView Fsr3Upscaler::viewFor(Res r, uint32_t parity) const {
    // Ping-pong (cpp:867-878): em frame PAR o SRV (historico) e' o _1 e o UAV o _2.
    const uint32_t srv = parity, uav = parity ^ 1u;
    switch (r) {
        case Res::InColor: return m_inColor;
        case Res::InDepth: return m_inDepth;
        case Res::InMotion: return m_inMotion;
        case Res::InExposure: return m_defaultExposure.view;
        case Res::InReactive: return m_inReactive ? m_inReactive : m_defaultReactive.view;
        case Res::InTransparency: return m_defaultReactive.view;
        case Res::DilatedMotion: return m_dilatedMotion.view;
        case Res::DilatedDepth: return m_dilatedDepth.view;
        case Res::ReconPrevDepth: return m_reconPrevDepth.view;
        case Res::Intermediate16: return m_intermediate16.view;
        case Res::CurrentLuma: return m_luma[srv].view;
        case Res::PreviousLuma: return m_luma[uav].view;
        case Res::SpdAtomic: return m_spdAtomic.view;
        case Res::FrameInfo: return m_frameInfo.view;
        case Res::SpdMip0: return m_spdMips.mipViews[0];
        case Res::SpdMip1: return m_spdMips.mipViews[1];
        case Res::SpdMip2: return m_spdMips.mipViews[2];
        case Res::SpdMip3: return m_spdMips.mipViews[3];
        case Res::SpdMip4: return m_spdMips.mipViews[4];
        case Res::SpdMip5: return m_spdMips.mipViews[5];
        case Res::SpdMipsAll: return m_spdMips.view;
        case Res::FarthestMip1: return m_farthestMip1.view;
        case Res::ShadingChangeImg: return m_shadingChange.view;
        case Res::DilatedReactive: return m_dilatedReactive.view;
        case Res::NewLocks: return m_newLocks.view;
        case Res::AccumSrv: return m_accum[srv].view;
        case Res::AccumUav: return m_accum[uav].view;
        case Res::LumaHistSrv: return m_lumaHist[srv].view;
        case Res::LumaHistUav: return m_lumaHist[uav].view;
        case Res::UpscaledSrv: return m_upscaled[srv].view;
        case Res::UpscaledUav: return m_upscaled[uav].view;
        case Res::LanczosLut: return m_lanczosLut.view;
        case Res::Output: return m_output.view;
        default: return VK_NULL_HANDLE;
    }
}

VkImageLayout Fsr3Upscaler::layoutFor(Res r) const {
    switch (r) {
        case Res::InColor:
        case Res::InDepth:
        case Res::InMotion:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case Res::InReactive:
            return m_inReactive ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        default:
            return VK_IMAGE_LAYOUT_GENERAL;
    }
}

void Fsr3Upscaler::writeSets() {
    if (!m_ctx || m_pool == VK_NULL_HANDLE || m_output.view == VK_NULL_HANDLE) return;
    if (!m_inColor || !m_inDepth || !m_inMotion) return;
    vkDeviceWaitIdle(m_ctx->device());
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkDescriptorBufferInfo> buffers;
    images.reserve(PassCount * 2 * 16);
    buffers.reserve(PassCount * 2 * 2);
    for (PassObjects& pass : m_passes) {
        for (uint32_t parity = 0; parity < 2; ++parity) {
            for (const Binding& b : pass.bindings) {
                VkWriteDescriptorSet w{};
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = pass.sets[parity];
                w.dstBinding = b.binding;
                w.descriptorCount = 1;
                w.descriptorType = b.type;
                if (b.type == kUbo) {
                    VkDescriptorBufferInfo bi{};
                    bi.buffer = m_ubo;
                    bi.offset = b.res == Res::Cb0 ? 0 : b.res == Res::CbSpd ? m_uboStride : 2 * m_uboStride;
                    bi.range = m_uboStride;
                    buffers.push_back(bi);
                    w.pBufferInfo = &buffers.back();
                } else {
                    VkDescriptorImageInfo ii{};
                    ii.imageView = viewFor(b.res, parity);
                    ii.imageLayout = layoutFor(b.res);
                    images.push_back(ii);
                    w.pImageInfo = &images.back();
                }
                writes.push_back(w);
            }
        }
    }
    vkUpdateDescriptorSets(m_ctx->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

// ---------------------------------------------------------------------------
// Frame

void Fsr3Upscaler::dispatch(VkCommandBuffer cmd, const FrameParams& params) {
    if (!m_ctx || !m_inColor || !m_inDepth || !m_inMotion) return;
    const uint32_t rw = m_renderW, rh = m_renderH, dw = m_displayW, dh = m_displayH;

    // ---- constantes (cpp:943-1031) ----
    const bool reset = params.reset || m_firstExecution;
    const bool first = m_firstExecution;
    m_firstExecution = false;
    Constants& C = m_c;
    C.previousFrameJitterOffset[0] = C.jitterOffset[0];
    C.previousFrameJitterOffset[1] = C.jitterOffset[1];
    C.jitterOffset[0] = params.jitterPx.x;
    C.jitterOffset[1] = params.jitterPx.y;
    C.previousFrameRenderSize[0] = C.renderSize[0];
    C.previousFrameRenderSize[1] = C.renderSize[1];
    C.renderSize[0] = int32_t(rw);
    C.renderSize[1] = int32_t(rh);
    C.maxRenderSize[0] = int32_t(rw);
    C.maxRenderSize[1] = int32_t(rh);
    const float aspect = float(rw) / float(rh);
    C.tanHalfFOV = std::tan(std::atan(std::tan(params.fovY * 0.5f) * aspect));
    C.viewSpaceToMetersFactor = params.metersPerUnit > 0.0f ? params.metersPerUnit : 1.0f;
    // deviceToViewDepth para depth 0..1 nao invertido e far finito (cpp:708-757).
    const float n = std::min(params.nearZ, params.farZ), f = std::max(params.nearZ, params.farZ);
    const float Q = f / (n - f);
    C.deviceToViewDepth[0] = -Q;
    C.deviceToViewDepth[1] = Q * n;
    const float tanHalf = std::tan(0.5f * params.fovY);
    C.deviceToViewDepth[2] = tanHalf * aspect;
    C.deviceToViewDepth[3] = tanHalf;
    C.previousFrameUpscaleSize[0] = C.upscaleSize[0];
    C.previousFrameUpscaleSize[1] = C.upscaleSize[1];
    C.upscaleSize[0] = int32_t(dw);
    C.upscaleSize[1] = int32_t(dh);
    C.downscaleFactor[0] = float(rw) / float(dw);
    C.downscaleFactor[1] = float(rh) / float(dh);
    C.deltaPreExposure = 1.0f;
    m_prevPreExposure = m_preExposure;
    m_preExposure = params.preExposure != 0.0f ? params.preExposure : 1.0f;
    if (m_prevPreExposure > 0.0f) C.deltaPreExposure = m_preExposure / m_prevPreExposure;
    // Nossos MVs ja' estao em UV: escala (rw, rh) dividida por renderSize = 1.
    C.motionVectorScale[0] = 1.0f;
    C.motionVectorScale[1] = 1.0f;
    C.motionVectorJitterCancellation[0] = C.motionVectorJitterCancellation[1] = 0.0f;
    const float phase = float(jitterPhaseCount(rw, dw));
    if (reset || C.jitterPhaseCount == 0.0f) {
        C.jitterPhaseCount = phase;
    } else if (phase > C.jitterPhaseCount) {
        C.jitterPhaseCount += 1.0f;
    } else if (phase < C.jitterPhaseCount) {
        C.jitterPhaseCount -= 1.0f;
    }
    C.deltaTime = std::clamp(params.frameTimeMs / 1000.0f, 0.0f, 1.0f);
    C.frameIndex = reset ? 0.0f : C.frameIndex + 1.0f;

    SpdConstants spd{};
    const uint32_t spdGx = (rw - 1) / 64 + 1, spdGy = (rh - 1) / 64 + 1;
    spd.numWorkGroups = spdGx * spdGy;
    spd.mips = static_cast<uint32_t>(std::min(std::floor(std::log2(float(std::max(rw, rh)))), 12.0f));
    spd.renderSize[0] = rw;
    spd.renderSize[1] = rh;

    const bool sharpen = params.sharpness > 0.0f;
    RcasConstants rcas{};
    {
        const float s = std::exp2(-(-2.0f * std::clamp(params.sharpness, 0.0f, 1.0f) + 2.0f));
        std::memcpy(&rcas.rcasConfig[0], &s, sizeof(float));
        rcas.rcasConfig[1] = glm::packHalf2x16(glm::vec2(s));
    }

    const uint32_t frame = m_ctx->currentFrame();
    const uint32_t base = static_cast<uint32_t>(frame * 3 * m_uboStride);
    std::memcpy(m_uboMapped + base, &C, sizeof(C));
    std::memcpy(m_uboMapped + base + m_uboStride, &spd, sizeof(spd));
    std::memcpy(m_uboMapped + base + 2 * m_uboStride, &rcas, sizeof(rcas));
    vmaFlushAllocation(m_ctx->allocator(), m_uboAlloc, base, 3 * m_uboStride);

    const uint32_t parity = m_resourceFrameIndex & 1u;

    // ---- barreira de entrada + saida de volta a GENERAL ----
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    if (m_outputLayout != VK_IMAGE_LAYOUT_GENERAL) {
        m_ctx->cmdImageBarrier(cmd, m_output.image, m_outputLayout, VK_IMAGE_LAYOUT_GENERAL,
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
        m_outputLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // ---- limpezas (secao 1, C1..C8) ----
    if (first) {
        for (int i = 0; i < 2; ++i) {
            clearColor(cmd, m_accum[i].image, clearFloat(0.0f));
            clearColor(cmd, m_luma[i].image, clearFloat(0.0f));
        }
    }
    if (reset) {
        clearColor(cmd, m_accum[parity].image, clearFloat(0.0f));
        clearColor(cmd, m_frameInfo.image, clearFloat(-1.0f, 1.0f, 0.0f, 0.0f));
    }
    // 0x3F800000 = bits de 1.0f: o shader le' o R32_UINT com uintBitsToFloat.
    clearColor(cmd, m_reconPrevDepth.image, clearUint(0x3F800000u));
    clearColor(cmd, m_spdAtomic.image, clearUint(0));
    clearColor(cmd, m_spdMips.image, clearFloat(0.0f), m_spdMips.mips);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    auto run = [&](Pass p, uint32_t gx, uint32_t gy, bool alt = false) {
        const PassObjects& pass = m_passes[p];
        uint32_t offsets[2];
        uint32_t nOff = 0;
        for (const Binding& b : pass.bindings) { // em ordem de binding, como o Vulkan exige
            if (b.type != kUbo) continue;
            offsets[nOff++] = base + (b.res == Res::Cb0 ? 0u : b.res == Res::CbSpd ? uint32_t(m_uboStride)
                                                                                  : uint32_t(2 * m_uboStride));
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, alt ? pass.pipelineAlt : pass.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass.layout, 0, 1, &pass.sets[parity],
                                nOff, offsets);
        vkCmdDispatch(cmd, gx, gy, 1);
        computeToCompute(cmd);
    };

    run(PrepareInputs, divUp(rw, 8), divUp(rh, 8));
    run(LumaPyramid, spdGx, spdGy);
    run(ShadingChangePyramid, spdGx, spdGy);
    run(ShadingChange, divUp(rw / 2, 8), divUp(rh / 2, 8));
    run(PrepareReactivity, divUp(rw, 8), divUp(rh, 8));
    run(LumaInstability, divUp(rw, 8), divUp(rh, 8));
    run(Accumulate, divUp(dw, 8), divUp(dh, 8), sharpen);
    if (sharpen) run(Rcas, divUp(dw, 16), divUp(dh, 16));
    if (params.debugView) run(DebugView, divUp(dw, 8), divUp(dh, 8));

    m_resourceFrameIndex = (m_resourceFrameIndex + 1) % 16;

    m_ctx->cmdImageBarrier(cmd, m_output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    m_outputLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

} // namespace eruption
