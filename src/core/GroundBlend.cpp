// JUNCAO MALHA x TERRENO. Peca que encosta no chao (pedra, coluna, escada)
// mistura, numa faixa de altura acima do chao, com o MESMO material do chao
// naquele XZ: as camadas do splat pela mascara, a cor base, a variacao macro, e
// a normal do chao (gbuffer/ground_blend.glsl). Sem isso a linha de contato e'
// uma troca dura de material e de normal. Tecnica: material do terreno em
// espaco de mundo amostrado pela malha perto do chao, com a normal puxada para
// a dele (DICE, "Photogrammetry and Star Wars Battlefront", GDC 2016; o Runtime
// Virtual Texture da Unreal faz o mesmo com o chao assado numa textura).
//
// Aqui: a textura de altura do chao (R16, a grade do .ter, montada uma vez por
// mapa) e as constantes do chao no FrameUBO a cada quadro.
#include "core/Engine.hpp"
#include "core/EngineInternal.hpp"
#include "core/Logger.hpp"
#include "utils/Profiler.hpp"
#include <vk_mem_alloc.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace eruption {

void Engine::setGroundBlend(float band, float fraction) {
    m_groundBlendBand = std::max(0.0f, band);
    m_groundBlendFraction = std::clamp(fraction, 0.01f, 1.0f);
}

// A grade do .ter em R16 UNORM: um texel por vertice da grade, (w+1) x (d+1),
// a mesma interpolacao de TerrainParser::getTerrainHeightAt com o filtro
// bilinear. R16 e nao R32F: celular nao filtra R32F linear (AGENTS.md).
bool Engine::buildGroundHeightTexture() {
    m_groundHeight = {};
    if (!m_currentMap) return false;
    // A altura da PROPRIA malha de chao quando ela e' uma grade (a superficie
    // que se ve); senao, a grade do .ter. Medido no primeiro andar do projeto
    // de teste: o .ter passa alguns u acima da malha, e com ele as pedras
    // baixas inteiras ficavam "dentro" da faixa e sumiam no chao.
    const auto& gb = m_modelRenderer.groundBlend(!m_currentMap->terrainFromFile);
    uint32_t vw = 0, vd = 0;
    float x0 = 0.0f, z0 = 0.0f, cellX = 10.0f, cellZ = 10.0f;
    std::vector<float> h;
    if (!gb.heights.empty()) {
        vw = gb.nx; vd = gb.nz;
        x0 = gb.x0; z0 = gb.z0; cellX = gb.dx; cellZ = gb.dz;
        h = gb.heights;
    } else {
        if (!m_currentMap->terrainFromFile) {
            ERUPTION_LOG_WARN("[JUNCAO] chao sem grade e mapa sem .ter: sem juncao");
            return false;
        }
        const TerrainFile& t = m_currentMap->terrain;
        const uint32_t w = t.width, d = t.height;
        if (w == 0 || d == 0 || t.cubes.size() < size_t(w) * d) return false;
        vw = w + 1; vd = d + 1;
        x0 = t.offsetX; z0 = t.offsetZ;
        cellX = cellZ = t.scale > 0.0f ? t.scale : 10.0f;
        h.resize(size_t(vw) * vd);
        for (uint32_t z = 0; z < vd; ++z)
            for (uint32_t x = 0; x < vw; ++x) {
                const uint32_t cx = std::min(x, w - 1), cz = std::min(z, d - 1);
                const int corner = (x == w ? 1 : 0) + (z == d ? 2 : 0); // SW, SE, NW, NE
                h[size_t(z) * vw + x] = -t.cubes[size_t(cz) * w + cx].height[corner];
            }
    }
    float lo = 1e30f, hi = -1e30f;
    for (float y : h) { lo = std::min(lo, y); hi = std::max(hi, y); }
    const float range = std::max(hi - lo, 1e-3f);
    EtexData e;
    e.vkFormat = VK_FORMAT_R16_UNORM;
    e.width = vw;
    e.height = vd;
    e.mipCount = 1;
    e.payload.resize(h.size() * 2);
    for (size_t i = 0; i < h.size(); ++i) {
        const uint16_t q = static_cast<uint16_t>(std::lround(std::clamp((h[i] - lo) / range, 0.0f, 1.0f) * 65535.0f));
        std::memcpy(&e.payload[i * 2], &q, 2);
    }
    e.mipSizes = {static_cast<uint32_t>(e.payload.size())};

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16_UNORM;
    imageInfo.extent = {vw, vd, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    if (vmaCreateImage(m_vulkan.allocator(), &imageInfo, &allocCI, &image, &alloc, nullptr) != VK_SUCCESS) return false;
    PROFILE_VRAM_ALLOC(ProfilerCategory::Textures, m_vulkan.allocationSize(alloc));
    VkBuffer staging;
    VmaAllocation stagingAlloc;
    m_vulkan.createBuffer(e.payload.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, staging, stagingAlloc);
    void* mapped = nullptr;
    vmaMapMemory(m_vulkan.allocator(), stagingAlloc, &mapped);
    std::memcpy(mapped, e.payload.data(), e.payload.size());
    vmaUnmapMemory(m_vulkan.allocator(), stagingAlloc);
    s_pendingUploads.push_back({image, alloc, staging, stagingAlloc, e.width, e.height, e.mipCount, e.mipSizes});
    VkImageView view;
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    vkc::createImageView(m_vulkan.device(), &viewInfo, nullptr, &view);
    const uint32_t slot = m_bindless.allocateSlotSafe();
    if (slot == 0) return false;
    m_bindless.updateTextureSafe(slot, view, m_defaultSampler);
    // O cache de assets e' dono da textura (libera na troca de mapa e no fim).
    ModelTextureGPU tex{};
    tex.image = image;
    tex.alloc = alloc;
    tex.view = view;
    tex.bindlessSlot = slot;
    tex.width = vw;
    tex.height = vd;
    tex.mipLevels = 1;
    char name[96];
    std::snprintf(name, sizeof(name), "__altura_do_chao__#%u", m_modelRenderer.instanceEpoch());
    m_assetCache.addModelTexture(name, tex);
    flushPendingUploads();
    // Texel i no vertice i: uv = (x - x0) / (cell * vw) + 0,5 / vw.
    m_groundHeight.slot = slot;
    m_groundHeight.map = Vec4(1.0f / (cellX * vw), 0.5f / vw - x0 / (cellX * vw),
                              1.0f / (cellZ * vd), 0.5f / vd - z0 / (cellZ * vd));
    m_groundHeight.minY = lo;
    m_groundHeight.range = range;
    m_groundHeight.cell = std::max(cellX, cellZ);
    ERUPTION_LOG_WARN("[JUNCAO] altura do chao %ux%u (%.1f a %.1f), %s, slot %u", vw, vd, lo, hi,
                      gb.heights.empty() ? "grade do .ter" : "malha do chao", slot);
    return true;
}

// Constantes do chao para gbuffer/ground_blend.glsl (FrameUBO::gb*). Faixa 0
// desliga; ERUPTION_TEST_GROUND_BLEND=<faixa>[,<fracao>] liga sem script (teste).
void Engine::fillGroundBlend(FrameUBO& ubo) {
    std::memset(&ubo.gbMaskU, 0, sizeof(Vec4) * 5 + sizeof(uint32_t) * 12);
    static const Vec2 kEnv = [] {
        Vec2 v(-1.0f, 0.3f);
        if (const char* e = std::getenv("ERUPTION_TEST_GROUND_BLEND")) std::sscanf(e, "%f,%f", &v.x, &v.y);
        return v;
    }();
    const float band = kEnv.x >= 0.0f ? kEnv.x : m_groundBlendBand;
    if (band <= 0.0f || !m_currentMap) return;
    const auto& g = m_modelRenderer.groundBlend(!m_currentMap->terrainFromFile);
    if (!g.valid) return;
    // Uma vez por mapa (a lista de instancias recomeca a cada carga).
    if (m_groundHeightEpoch != m_modelRenderer.instanceEpoch() + 1u) {
        m_groundHeightEpoch = m_modelRenderer.instanceEpoch() + 1u;
        buildGroundHeightTexture();
    }
    if (m_groundHeight.slot == 0) return;
    ubo.gbMaskU = Vec4(g.maskU, band);
    ubo.gbMaskV = Vec4(g.maskV, m_groundHeight.cell);
    ubo.gbUvU = Vec4(g.uvU, m_groundHeight.minY);
    ubo.gbUvV = Vec4(g.uvV, m_groundHeight.range);
    ubo.gbHeight = m_groundHeight.map;
    for (int i = 0; i < 4; ++i) ubo.gbSplat[i] = g.splat[i];
    ubo.gbSlots[0] = g.mask;
    ubo.gbSlots[1] = g.mask2;
    ubo.gbSlots[2] = m_groundHeight.slot;
    ubo.gbSlots[3] = g.color;
    ubo.gbSlots2[0] = g.normal;
    ubo.gbSlots2[1] = g.pbr;
    // ERUPTION_TEST_GROUND_BLEND_DEBUG=1|2|3: ver ground_blend.glsl.
    static const uint32_t kDebug = [] {
        const char* e = std::getenv("ERUPTION_TEST_GROUND_BLEND_DEBUG");
        return e ? static_cast<uint32_t>(std::atoi(e)) : 0u;
    }();
    ubo.gbSlots2[2] = kDebug;
    const float fracao = kEnv.x >= 0.0f ? std::clamp(kEnv.y, 0.01f, 1.0f) : m_groundBlendFraction;
    std::memcpy(&ubo.gbSlots2[3], &fracao, sizeof(float));
}

} // namespace eruption
