// Minimapa do Engine: criacao do alvo, composicao (terreno + agua + marcadores
// em projecao ortografica do mapa inteiro), cache dos limites de agua e o
// passe de desenho. Saiu do Engine.cpp em 2026-09-04, na quebra do arquivo.
//
// O passe roda com LOD BASE forcado de proposito: e' ortografico e cobre o
// mapa inteiro, entao distancia de camera - que e' o criterio do LOD - nao
// tem relacao nenhuma com tamanho na tela aqui.

#include "core/Engine.hpp"
#include "core/EngineInternal.hpp"
#include "game/PlayerController.hpp"
#include "core/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/ImageUtils.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "formats/MapLoader.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <vk_mem_alloc.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace eruption {

void Engine::initMinimap() {
    bool ok = m_vulkan.createImage(MINIMAP_RES, MINIMAP_RES, VK_FORMAT_R8G8B8A8_UNORM, 
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 
        VMA_MEMORY_USAGE_GPU_ONLY, m_minimapImage, m_minimapAlloc);
    if (!ok) { ERUPTION_LOG_ERROR("Failed to create minimap color image"); return; }
    VkImageViewCreateInfo viewInfo{}; viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_minimapImage; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; viewInfo.subresourceRange.baseMipLevel = 0; viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0; viewInfo.subresourceRange.layerCount = 1;
    vkc::createImageView(m_vulkan.device(), &viewInfo, nullptr, &m_minimapView);
    m_minimapDescriptorSet = ImGui_ImplVulkan_AddTexture(m_defaultSampler, m_minimapView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    initMinimapComposite();
}

// Builds the fullscreen pass that turns the minimap G-buffer into the final
// minimap image. Before this, renderMinimap() blitted the raw ALBEDO attachment
// straight to the minimap texture -- pre-lighting colour, which is why the
// minimap had no shadow (shadow is produced by the deferred lighting pass, which
// never runs for the minimap camera) and no water (water is a forward pass drawn
// after lighting, into the lit target, so it never reaches the G-buffer).
void Engine::initMinimapComposite() {
    VkDescriptorSetLayoutBinding bindings[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 4;
    dslInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(m_vulkan.device(), &dslInfo, nullptr, &m_minimapCompositeSetLayout) != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("[MINIMAP] failed to create composite descriptor set layout");
        return;
    }

    // 128 bytes exactly: one mat4 + four vec4. Keeping it at the guaranteed
    // minimum means the shader recovers its world->UV basis from invViewProj
    // instead of also receiving the forward matrix.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(Mat4) + 5 * sizeof(Vec4);

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &m_minimapCompositeSetLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcRange;
    if (vkCreatePipelineLayout(m_vulkan.device(), &plInfo, nullptr, &m_minimapCompositeLayout) != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("[MINIMAP] failed to create composite pipeline layout");
        return;
    }

    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = m_imguiPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &m_minimapCompositeSetLayout;
    if (vkAllocateDescriptorSets(m_vulkan.device(), &dsAlloc, &m_minimapCompositeDS) != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("[MINIMAP] failed to allocate composite descriptor set");
        m_minimapCompositeDS = VK_NULL_HANDLE;
        return;
    }

    auto vertCode = ShaderCompiler::loadSPIRV("postprocess/fullscreen.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("postprocess/minimap_composite.frag.spv");
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_ERROR("[MINIMAP] composite shaders missing; minimap stays unlit");
        return;
    }
    auto createMod = [&](const std::vector<uint32_t>& code) {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size() * sizeof(uint32_t);
        ci.pCode = code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(m_vulkan.device(), &ci, nullptr, &mod);
        return mod;
    };
    VkShaderModule vMod = createMod(vertCode);
    VkShaderModule fMod = createMod(fragCode);
    if (vMod != VK_NULL_HANDLE && fMod != VK_NULL_HANDLE) {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vMod; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fMod; stages[1].pName = "main";

        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.blendEnable = VK_FALSE;

        PipelineBuilder builder;
        builder.setShaderStages({stages[0], stages[1]})
               .setLayout(m_minimapCompositeLayout)
               .setColorAttachmentFormats({VK_FORMAT_R8G8B8A8_UNORM})
               .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
               .setPolygonMode(VK_POLYGON_MODE_FILL)
               .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
               .setBlendState({blend})
               .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR});
        m_minimapCompositePipeline = builder.build(m_vulkan.device());
    }
    if (vMod != VK_NULL_HANDLE) vkDestroyShaderModule(m_vulkan.device(), vMod, nullptr);
    if (fMod != VK_NULL_HANDLE) vkDestroyShaderModule(m_vulkan.device(), fMod, nullptr);

    if (m_minimapCompositePipeline == VK_NULL_HANDLE) {
        ERUPTION_LOG_ERROR("[MINIMAP] composite pipeline build failed; minimap stays unlit");
    }
}

void Engine::shutdownMinimap() {
    if (m_minimapCompositePipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_vulkan.device(), m_minimapCompositePipeline, nullptr);
    if (m_minimapCompositeLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_vulkan.device(), m_minimapCompositeLayout, nullptr);
    if (m_minimapCompositeSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_vulkan.device(), m_minimapCompositeSetLayout, nullptr);
    if (m_minimapView != VK_NULL_HANDLE) vkDestroyImageView(m_vulkan.device(), m_minimapView, nullptr);
    if (m_minimapImage != VK_NULL_HANDLE) vmaDestroyImage(m_vulkan.allocator(), m_minimapImage, m_minimapAlloc);
}

void Engine::cacheWaterBounds(const WaterMesh& mesh) {
    // AGUA ENTERRADA: se o ponto MAIS BAIXO do terreno ainda esta' acima da
    // superficie da agua, ela nao pode ser vista de lugar nenhum - esta'
    // soterrada. E' o caso de cidade-A: o .ter declara UM plano global
    // cobrindo o mapa inteiro (AABB 1560x1960) em Y=-23, com o nivel em
    // -25.8, enquanto a cidade fica acima. O teste de frustum NAO pega isso
    // (um plano desse tamanho esta' sempre dentro do frustum), e a agua
    // cobrava 1,6-2,0 ms de CPU por frame - ceu-para-textura + espuma +
    // copia de refracao - para nunca aparecer. Reportado pelo autor
    // 2026-09-02 ("a agua nao ta na tela de cidade-A e mesmo assim ta
    // consumindo tempo").
    //
    // O teste roda UMA vez por (re)geracao de malha, custa uma varredura dos
    // cubos do terreno, e e' CONSERVADOR: qualquer canto de terreno abaixo do
    // nivel da agua ja' marca como potencialmente visivel.
    m_waterFullyBuried = false;
    if (m_currentMap) {
        const auto& ter = m_currentMap->terrain;
        float terrainMinY = FLT_MAX;
        for (const auto& c : ter.cubes) {
            terrainMinY = std::min({terrainMinY, c.height[0], c.height[1],
                                    c.height[2], c.height[3]});
        }
        const float waterY = m_mapBaseWaterLevel + m_waterMenu.config.waterLevel;
        if (terrainMinY < FLT_MAX && waterY < terrainMinY - 0.5f) {
            m_waterFullyBuried = true;
            ERUPTION_LOG_WARN("Agua SOTERRADA (nivel %.1f abaixo do terreno mais baixo %.1f): "
                              "passe de agua, ceu-para-textura e refracao desligados neste mapa",
                              waterY, terrainMinY);
        }
    }

    // AABB horizontal da malha de agua, calculado UMA vez por (re)geracao.
    // O Y e' ignorado aqui: a altura vem do waterLevel no momento do teste,
    // porque o nivel e' ajustavel em runtime pelo menu.
    m_waterBoundsValid = false;
    if (mesh.vertices.empty()) return;
    Vec3 mn(FLT_MAX), mx(-FLT_MAX);
    for (const auto& v : mesh.vertices) {
        mn = glm::min(mn, v.position);
        mx = glm::max(mx, v.position);
    }
    m_waterAabbMin = mn;
    m_waterAabbMax = mx;
    m_waterBoundsValid = true;
}

void Engine::renderMinimap(VkCommandBuffer cmd) {
    if (!m_currentMap || m_minimapImage == VK_NULL_HANDLE) return;

    // O MINIMAPA E' ESTATICO: a camera dele e' ortografica, fixa no centro do
    // mapa (mapCenter vem das dimensoes do terreno, nao do jogador) e olhando
    // para baixo. Ainda assim ele redesenhava O TERRENO INTEIRO E TODOS OS
    // MODELOS a cada frame, dentro do bloco do G-buffer - era o grosso dos
    // 4,9 ms de CPU dessa fase, com o passe de modelos em si custando 0,358 ms.
    //
    // Agora so' redesenha quando o conteudo pode ter mudado: mapa diferente, ou
    // numero de instancias/chunks diferente (o carregamento e' progressivo em
    // segundo plano, entao a cena cresce depois do swap - por isso nao basta
    // desenhar uma vez so' e parar).
    const void* mapKey = static_cast<const void*>(m_currentMap.get());
    const size_t instCount = m_modelRenderer.getInstances().size();
    const size_t chunkCount = m_terrainRenderer.chunks().size();
    const bool dirty = m_minimapForce || mapKey != m_minimapKey ||
                       instCount != m_minimapInstances || chunkCount != m_minimapChunks;
    if (!dirty) return;
    m_minimapKey = mapKey;
    m_minimapInstances = instCount;
    m_minimapChunks = chunkCount;
    m_gbuffer.beginPass(cmd);
    // A cena do minimapa e' desenhada num QUADRADO GRANDE do G-buffer e so'
    // depois reduzida para MINIMAP_RES pelo composite, em vez de rasterizada
    // direto em 256x256.
    //
    // O quadrado importa porque a projecao e' ortografica e simetrica: um
    // viewport nao-quadrado esticaria o mapa. O TAMANHO importa por dois
    // motivos: rasterizar o mapa inteiro em 256x256 joga fora quase toda a
    // geometria fina (uma casa vira meio pixel), e a reducao de 1024 para 256
    // no composite vira supersampling 4x4 de graca - o minimapa fica
    // legivelmente mais nitido do que rasterizando direto no tamanho final.
    // Custo: pago so' quando o minimapa e' reassado (o `dirty` acima), nao por
    // frame.
    const uint32_t miniRender = std::min(m_gbuffer.extent().width, m_gbuffer.extent().height);
    VkViewport viewport{}; viewport.x = 0; viewport.y = 0; viewport.width = (float)miniRender; viewport.height = (float)miniRender; viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f; vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{}; scissor.offset = {0, 0}; scissor.extent.width = miniRender; scissor.extent.height = miniRender; vkCmdSetScissor(cmd, 0, 1, &scissor);
    // Frame the ortho on the geometry that is ACTUALLY going to be drawn, by
    // taking the union of the terrain chunk and model instance AABBs.
    //
    // This used to derive the extent arithmetically as `terrain.width * 10`,
    // centred at `width * 0.5`. Both halves of that were wrong:
    //   - the 10 units/cell scale is not universal (maps carry their own
    //     scale; the medieval village is 5x), so on parana_field the assumed
    //     extent was nearly twice the real one and the map occupied only the
    //     top half of the minimap with the rest black - the bug PIROCLASTO
    //     reported;
    //   - it assumed the terrain starts at the world origin, but the .ter
    //     carries an offset (parana_field's is (-55.02, -45.91)).
    // Measuring the real bounds removes both guesses at once and works for
    // GLB-only maps, which have no .ter dimensions to derive anything from.
    float zoom = 600.0f;
    Vec3 mapCenter = m_playerController->pos();
    {
        Vec3 bmin( std::numeric_limits<float>::max());
        Vec3 bmax(-std::numeric_limits<float>::max());
        bool any = false;
        for (const auto& c : m_terrainRenderer.chunks()) {
            if (c.indexCount == 0) continue;
            bmin = glm::min(bmin, c.aabbMin);
            bmax = glm::max(bmax, c.aabbMax);
            any = true;
        }
        // Terrain alone is the right frame when it exists (props sit on it).
        // Fall back to the model instances for maps that are pure GLB geometry.
        if (!any) {
            // Only instances backed by a mesh that is ACTUALLY on the GPU.
            // `enabled` is not enough: the instance list is complete from map
            // load, but the meshes behind it upload for seconds afterwards, and
            // some never resolve at all. Those phantom instances still carry a
            // world AABB, and on parana_field they stretched the bound in +Z to
            // 2006 while the geometry that really draws ends around 1120. The
            // ortho then centred 443 units past the real middle and the map
            // rendered against the top edge with a black band below -- measured
            // as exactly the 54-row offset observed.
            const auto& meshes = m_modelRenderer.getMeshes();
            for (const auto& inst : m_modelRenderer.getInstances()) {
                if (!inst.enabled) continue;
                if (inst.meshIndex >= meshes.size()) continue;
                const auto& m = meshes[inst.meshIndex];
                if (m.vertexBuffer == VK_NULL_HANDLE || m.indexCount == 0) continue;
                bmin = glm::min(bmin, inst.worldAabbMin);
                bmax = glm::max(bmax, inst.worldAabbMax);
                any = true;
            }
        }
        if (any && bmax.x > bmin.x && bmax.z > bmin.z) {
            mapCenter.x = (bmin.x + bmax.x) * 0.5f;
            mapCenter.z = (bmin.z + bmax.z) * 0.5f;
            // Half-extent of the LARGER axis: the minimap image is square, so
            // fitting each axis independently would stretch a rectangular map.
            // Keeping the aspect leaves a transparent margin on the short axis,
            // which is what the framed minimap widget expects.
            zoom = glm::max(bmax.x - bmin.x, bmax.z - bmin.z) * 0.5f;
            zoom *= 1.02f; // hairline margin so the border texel is not clipped
        } else if (m_currentMap) {
            // Last resort, only when nothing has loaded yet.
            const auto& terr = m_currentMap->terrain;
            const float mapWidth = (float)terr.width * 10.0f;
            const float mapHeight = (float)terr.height * 10.0f;
            zoom = std::max(mapWidth, mapHeight) * 0.5f;
            mapCenter.x = terr.offsetX + mapWidth * 0.5f;
            mapCenter.z = terr.offsetZ + mapHeight * 0.5f;
        }
    }
    Mat4 proj = glm::ortho(-zoom, zoom, zoom, -zoom, -5000.0f, 5000.0f);
    // Up=(0,0,-1) makes East=Right, but South=Up. Ortho(z,-z) then makes North=Bottom
    Mat4 view = glm::lookAt(mapCenter + Vec3(0, 1000, 0), mapCenter, Vec3(0, 0, -1));
    Mat4 vp = proj * view;
    Frustum miniFrustum; miniFrustum.extractFromMatrix(vp);
    m_terrainRenderer.render(cmd, vp, miniFrustum);
    m_modelRenderer.setForceBaseLod(true);
    m_modelRenderer.setScreenMetrics(0.0f); // minimapa: ortografico, sem descarte por tela
    m_modelRenderer.render(cmd, vp, miniFrustum, mapCenter + Vec3(0, 1000, 0));
    m_modelRenderer.setForceBaseLod(false);
    if (std::getenv("ERUPTION_TEST_DUMP_MINIMAP")) {
        ERUPTION_LOG_WARN("[MINIMAP] nivelAgua=%.1f (base=%.1f + cfg=%.1f)",
                          m_mapBaseWaterLevel + m_waterMenu.config.waterLevel,
                          m_mapBaseWaterLevel, m_waterMenu.config.waterLevel);
        ERUPTION_LOG_WARN("[MINIMAP] passe: visiveis=%zu draws=%u tris=%u (de %zu instancias)",
                          m_modelRenderer.lastVisibleCount(), m_modelRenderer.lastDrawCalls(),
                          m_modelRenderer.lastDrawnTriangles(), m_modelRenderer.getInstances().size());
    }
    // NOTE: Sprites are NOT rendered in minimap pass.
    // Minimap markers are drawn as ImGui overlays by the application (PlayerController::renderMinimapMarkers).
    m_gbuffer.endPass(cmd);

    // The minimap G-buffer is drawn; now light it. This used to be a straight
    // blit of the ALBEDO attachment into the minimap image, which is why the
    // minimap showed neither shadow (born in the deferred lighting pass, which
    // never runs for this camera) nor water (a forward pass after lighting,
    // into the lit target -- it never touches this G-buffer).
    //
    // Both cost nothing per frame: this whole function is gated by `dirty`
    // above, so it runs on map change / progressive-load growth only. The
    // 4,9 ms -> 0,5 ms win from commit 1f8c4a5 is preserved.
    if (m_minimapCompositePipeline != VK_NULL_HANDLE && m_minimapCompositeDS != VK_NULL_HANDLE) {
        VkDescriptorImageInfo imgs[4] = {};
        imgs[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgs[0].imageView = m_gbuffer.albedoView();
        imgs[0].sampler = m_defaultSampler;
        imgs[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgs[1].imageView = m_gbuffer.normalView();
        imgs[1].sampler = m_defaultSampler;
        // Binding 2 continua declarado no shader (layout estavel), mas aponta
        // para o NORMAL, nao para o depth: o composite nao amostra mais
        // profundidade. Amostrar exigia uma transicao de layout que gerava 40
        // erros de validacao por execucao, porque o passe de fumaca deixa a
        // imagem em DEPTH_READ_ONLY_OPTIMAL e a barreira daqui assumia
        // DEPTH_ATTACHMENT_OPTIMAL - e transicao com oldLayout errado deixa o
        // conteudo indefinido.
        imgs[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgs[2].imageView = m_gbuffer.normalView();
        imgs[2].sampler = m_defaultSampler;
        // Altura de mundo: o canal que antes carregava um material id que
        // ninguem lia. E' a fonte de posicao vertical do minimapa, ja' que o
        // depth nao e' escrito de forma confiavel (G14).
        imgs[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgs[3].imageView = m_gbuffer.materialView();
        // R8_UINT (altura de mundo) nao tem FILTER_LINEAR no format feature:
        // sampler LINEAR aqui era VUID-vkCmdDraw-magFilter-04553 em todo frame
        // (usampler2D no shader - a leitura ja' era nearest de fato).
        imgs[3].sampler = m_nearestSampler;

        // So' reescreve o DS quando as views mudaram (recriacao do G-buffer):
        // atualizar todo re-bake enquanto o CB do frame anterior ainda esta'
        // pendente e' VUID-03047 (2 por frame). As views sao estaveis entre
        // resizes, entao a escrita e' quase sempre pulada.
        if (m_minimapCompositeDSAlbedoView != m_gbuffer.albedoView()) {
            VkWriteDescriptorSet writes[4] = {};
            for (uint32_t i = 0; i < 4; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = m_minimapCompositeDS;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &imgs[i];
            }
            vkUpdateDescriptorSets(m_vulkan.device(), 4, writes, 0, nullptr);
            m_minimapCompositeDSAlbedoView = m_gbuffer.albedoView();
        }

        m_vulkan.cmdImageBarrier(cmd, m_gbuffer.albedoImage(),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        m_vulkan.cmdImageBarrier(cmd, m_gbuffer.materialImage(),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        m_vulkan.cmdImageBarrier(cmd, m_gbuffer.normalImage(),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        // Sem barreira de profundidade: o composite nao le mais o depth.
        m_vulkan.cmdImageBarrier(cmd, m_minimapImage,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView = m_minimapView;
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkExtent2D miniExtent{MINIMAP_RES, MINIMAP_RES};
        m_vulkan.cmdBeginRendering(cmd, {colorAtt}, nullptr, nullptr, miniExtent);
        VkViewport cvp{}; cvp.x = 0; cvp.y = 0; cvp.width = (float)MINIMAP_RES; cvp.height = (float)MINIMAP_RES; cvp.minDepth = 0.0f; cvp.maxDepth = 1.0f;
        VkRect2D csc{}; csc.offset = {0, 0}; csc.extent.width = MINIMAP_RES; csc.extent.height = MINIMAP_RES;
        vkCmdSetViewport(cmd, 0, 1, &cvp);
        vkCmdSetScissor(cmd, 0, 1, &csc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_minimapCompositePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_minimapCompositeLayout,
                                0, 1, &m_minimapCompositeDS, 0, nullptr);

        struct MinimapPush {
            Mat4 invViewProj;
            Vec4 sunDir;
            Vec4 sunColor;
            Vec4 params;
            Vec4 waterColor;
            Vec4 uvScale;
        } mpush;
        mpush.invViewProj = glm::inverse(vp);
        // getSunDirection() is the direction the light TRAVELS; the shader wants
        // the direction toward the sun.
        Vec3 toSun = -glm::normalize(m_dayNightCycle.getSunDirection());
        mpush.sunDir = Vec4(toSun, glm::max(m_dayNightCycle.getSunIntensity(), 0.0f));
        mpush.sunColor = Vec4(m_dayNightCycle.getSunColor(), m_ambientIntensity);
        // Shadow ray length scales with the map: `zoom` is half the larger map
        // side, so a fixed metre count would vanish on a big map and swallow a
        // small one.
        const float shadowReach = glm::clamp(zoom * 0.05f, 20.0f, 400.0f);
        mpush.params = Vec4(m_mapBaseWaterLevel + m_waterMenu.config.waterLevel,
                            0.65f, 0.75f, shadowReach);
        // Deep-water tint and the depth over which the shoreline fades into it.
        mpush.waterColor = Vec4(0.09f, 0.24f, 0.38f, 40.0f);
        // The minimap occupies only the top-left MINIMAP_RES square of the
        // full-size scene G-buffer, so the composite has to sample that
        // sub-rect instead of the whole attachment.
        mpush.uvScale = Vec4(float(miniRender) / float(m_gbuffer.extent().width),
                             float(miniRender) / float(m_gbuffer.extent().height),
                             0.0f, 0.0f);
        vkCmdPushConstants(cmd, m_minimapCompositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(mpush), &mpush);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        m_vulkan.cmdEndRendering(cmd);

        m_vulkan.cmdImageBarrier(cmd, m_minimapImage,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        // The G-buffer images are left in SHADER_READ_ONLY. That is fine: the
        // next GBuffer::beginPass() transitions them from UNDEFINED.

        // Re-bake until the geometry actually stops changing.
        //
        // The instance COUNT is final almost immediately (the list is built at
        // map load), but the meshes behind those instances stream to the GPU
        // for seconds afterwards -- parana_field spends ~6 s just baking
        // embedded textures. Gating only on instance/chunk COUNT therefore
        // baked the minimap while most meshes still had no vertex buffer, and
        // never baked again: whole regions of the map were simply missing, in
        // a band that moved between runs because it followed load order. That
        // is the "minimapa preto" PIROCLASTO reported -- not framing, not
        // lighting, not culling.
        //
        // Triangles actually drawn is the honest readiness signal: it keeps
        // rising while meshes upload and goes flat when the scene is complete.
        // One extra bake after it settles costs nothing (this whole function
        // is already gated by `dirty`) and self-corrects for any future source
        // of late geometry.
        const uint32_t drawn = m_modelRenderer.lastDrawnTriangles();
        m_minimapForce = (drawn != m_minimapTriangles);
        m_minimapTriangles = drawn;
    } else {
        // Fallback to the old raw-albedo blit if the composite pipeline failed
        // to build, so a shader problem degrades the minimap instead of
        // removing it.
        m_vulkan.cmdImageBarrier(cmd, m_minimapImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        m_vulkan.cmdImageBarrier(cmd, m_gbuffer.albedoImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        m_vulkan.copyImageToImage(cmd, m_gbuffer.albedoImage(), m_minimapImage, MINIMAP_RES, MINIMAP_RES); // fallback: so' o canto, sem reducao
        m_vulkan.cmdImageBarrier(cmd, m_minimapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        const uint32_t drawnFb = m_modelRenderer.lastDrawnTriangles();
        m_minimapForce = (drawnFb != m_minimapTriangles);
        m_minimapTriangles = drawnFb;
    }
}

} // namespace eruption
