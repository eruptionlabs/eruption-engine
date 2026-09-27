// Tela de carregamento do Engine: splash com logo/silhueta/glow, barra de
// progresso, e o passe que apresenta tudo isso ENQUANTO o mapa carrega (fora
// do laco normal de frame, porque durante a carga o laco esta' bloqueado).
// Saiu do Engine.cpp em 2026-09-04, na quebra do arquivo.

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "utils/ImageUtils.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/PostFormat.hpp"
#include <stb_image.h>
#include <vk_mem_alloc.h>
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace eruption {

void Engine::presentLoadingScreen(const std::string& mapName, const std::string& task, float progress, bool clear) {
    float clampedProgress = std::clamp(progress, 0.0f, 1.0f);

    // Smooth followers for the two independent logo glows.
    // Task glow follows the current boot phase; bar glow follows the overall progress bar.
    if (clampedProgress > m_splashTaskProgress) {
        m_splashTaskProgress += (clampedProgress - m_splashTaskProgress) * 0.5f;
    }
    if (clampedProgress > m_splashBarProgress) {
        m_splashBarProgress += (clampedProgress - m_splashBarProgress) * 0.5f;
    }

    // Pump window events so the OS doesn't mark the window as "not responding"
    // during long synchronous initialization.
    if (m_vulkan.window()) glfwPollEvents();

    if (!m_vulkan.beginFrame()) return;
    VkCommandBuffer cmd = m_vulkan.currentCmdBuf();
    uint32_t imageIndex = m_vulkan.currentImageIndex();

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = m_vulkan.swapImageView(imageIndex);
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    if (clear) {
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.clearValue.color = {0.0f, 0.0f, 0.0f, 1.0f};
    } else {
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    }

    m_vulkan.cmdImageBarrier(cmd, m_vulkan.swapImage(imageIndex), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    m_vulkan.cmdBeginRendering(cmd, {colorAttachment}, nullptr, nullptr, m_vulkan.swapExtent());

    renderLoadingUI(mapName, task, progress);

    m_vulkan.cmdEndRendering(cmd);
    m_vulkan.cmdImageBarrier(cmd, m_vulkan.swapImage(imageIndex), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);

    m_vulkan.endFrame();
}

// ------------------------------------------------------------------
// Background loading helpers (frame CB upload)
// ------------------------------------------------------------------

void Engine::renderLoadingProgress() {
    // Show progress while there is an active background load OR a staging map not yet swapped
    bool hasActiveLoad = m_backgroundLoader && m_backgroundLoader->isLoading();
    bool hasStagingMap = m_stagingMapContext && (!m_stagingMapContext->isGpuReady() || m_mapSwapPending);
    // A weather cloud field still draining its spawn queue counts as loading:
    // spawn warm-up belongs behind the loading overlay, not as an in-game hitch.
    const bool hasPendingClouds = !m_pendingCloudSpawns.empty();
    if (!hasActiveLoad && !hasStagingMap && !hasPendingClouds) return;

    float progress = 0.0f;
    std::string task = "Loading...";
    std::string mapName = m_backgroundLoader ? m_backgroundLoader->currentMapName() : "";

    if (m_stagingMapContext) {
        progress = m_stagingMapContext->totalProgress();
        mapName = m_stagingMapContext->mapName;
        if (!m_stagingMapContext->isCpuReady()) task = "Parsing map data";
        else if (!m_stagingMapContext->isGpuReady()) task = "Uploading to GPU";
        else task = "Finalizing";
    } else if (hasActiveLoad) {
        progress = m_backgroundLoader->progress() * 0.5f;
        task = "Loading map data";
    } else {
        // Only cloud spawns pending: near-full bar until the field is up.
        progress = 0.9f;
        task = "Spawning weather";
    }

    // Smooth follower for the bottom loading bar progress shown by the logo glow.
    if (progress > m_splashBarProgress) {
        m_splashBarProgress += (progress - m_splashBarProgress) * 0.5f;
    }

    // Draw the existing Van Halen style bar in the bottom-right corner
    // We reuse renderLoadingUI logic but adapt it for in-frame HUD overlay
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    float width = 220.0f;
    float height = 6.0f;
    float margin = 40.0f;
    float rounding = 3.0f;

    ImVec2 pMax = ImVec2(displaySize.x - margin, displaySize.y - margin);
    ImVec2 pMin = ImVec2(pMax.x - width, pMax.y - height);

    ImU32 vhRed = IM_COL32(255, 60, 40, 255);
    ImU32 vhStripe = IM_COL32(255, 255, 255, 60);
    ImU32 blackBg = IM_COL32(15, 15, 15, 180);

    drawList->AddRectFilled(pMin, pMax, blackBg, rounding);

    float progressWidth = width * progress;
    if (progressWidth > 0.0f) {
        ImVec2 pFillMax = ImVec2(pMin.x + progressWidth, pMax.y);
        drawList->AddRectFilled(pMin, pFillMax, vhRed, rounding);
        drawList->PushClipRect(pMin, pFillMax, true);
        for (float x = pMin.x - 10.0f; x < pFillMax.x; x += 8.0f) {
            drawList->AddLine(ImVec2(x, pMin.y - 2), ImVec2(x + 8.0f, pMax.y + 2), vhStripe, 1.5f);
        }
        drawList->PopClipRect();
        drawList->AddLine(ImVec2(pMin.x + 1, pMin.y + 1), ImVec2(pFillMax.x - 1, pMin.y + 1), IM_COL32(255, 255, 255, 30), 1.0f);
    }

    auto drawStrokedText = [&](ImVec2 pos, ImU32 col, const char* text) {
        drawList->AddText(ImVec2(pos.x - 1, pos.y - 1), IM_COL32(0, 0, 0, 255), text);
        drawList->AddText(ImVec2(pos.x + 1, pos.y - 1), IM_COL32(0, 0, 0, 255), text);
        drawList->AddText(ImVec2(pos.x - 1, pos.y + 1), IM_COL32(0, 0, 0, 255), text);
        drawList->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 255), text);
        drawList->AddText(pos, col, text);
    };

    std::string progressPct = std::to_string((int)(progress * 100.0f)) + "%";
    ImVec2 pctSize = ImGui::CalcTextSize(progressPct.c_str());
    drawStrokedText(ImVec2(pMax.x - pctSize.x, pMin.y - pctSize.y - 5.0f), IM_COL32(200, 200, 200, 255), progressPct.c_str());

    if (!task.empty()) {
        float yOffset = -18.0f;
        if (!mapName.empty()) {
            drawStrokedText(ImVec2(pMin.x, pMin.y + yOffset - 15.0f), vhRed, mapName.c_str());
            drawStrokedText(ImVec2(pMin.x, pMin.y + yOffset), IM_COL32(150, 150, 150, 255), task.c_str());
        } else {
            drawStrokedText(ImVec2(pMin.x, pMin.y + yOffset), IM_COL32(150, 150, 150, 255), task.c_str());
        }
    }
}

// ------------------------------------------------------------------
// Synchronous map load (used for initial boot / fallback)
// ------------------------------------------------------------------



void Engine::initSplashLogo() {
    // 1. Load main logo and create texture
    if (createTextureFromFile("assets/icon/eruption_v3_512.png", m_splashLogo)) {
        m_splashLogoDescriptorSet = ImGui_ImplVulkan_AddTexture(m_defaultSampler, m_splashLogo.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
        ERUPTION_LOG_ERROR("Failed to load splash logo assets/icon/eruption_v3_512.png");
    }

    // 2. Create silhouette from main logo's pixels
    int w, h, channels;
    unsigned char* pixels = stbi_load("assets/icon/eruption_v3_512.png", &w, &h, &channels, STBI_rgb_alpha);
    if (pixels) {
        std::vector<unsigned char> silhouette_pixels(w * h * 4);
        for (int i = 0; i < w * h; ++i) {
            if (pixels[i * 4 + 3] > 20) {
                silhouette_pixels[i * 4 + 0] = 255;
                silhouette_pixels[i * 4 + 1] = 255;
                silhouette_pixels[i * 4 + 2] = 255;
                silhouette_pixels[i * 4 + 3] = 255;
            } else {
                silhouette_pixels[i * 4 + 0] = 0;
                silhouette_pixels[i * 4 + 1] = 0;
                silhouette_pixels[i * 4 + 2] = 0;
                silhouette_pixels[i * 4 + 3] = 0;
            }
        }
        stbi_image_free(pixels);

        if (createTextureFromPixels(silhouette_pixels.data(), w, h, m_splashSilhouette)) {
            m_splashSilhouetteDescriptorSet = ImGui_ImplVulkan_AddTexture(m_defaultSampler, m_splashSilhouette.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        // 2b. Gaussian blur the silhouette alpha to create a soft glow texture
        {
            int radius = 28;
            float sigma = 10.0f;
            std::vector<float> kernel(2 * radius + 1);
            float sum = 0.0f;
            for (int i = -radius; i <= radius; ++i) {
                float v = std::exp(-(float)(i * i) / (2.0f * sigma * sigma));
                kernel[i + radius] = v;
                sum += v;
            }
            for (float& v : kernel) v /= sum;

            std::vector<float> src(w * h), tmp(w * h);
            for (int i = 0; i < w * h; ++i) src[i] = silhouette_pixels[i * 4 + 3] / 255.0f;

            // Horizontal pass
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    float acc = 0.0f;
                    for (int k = -radius; k <= radius; ++k) {
                        int sx = std::clamp(x + k, 0, w - 1);
                        acc += src[y * w + sx] * kernel[k + radius];
                    }
                    tmp[y * w + x] = acc;
                }
            }
            // Vertical pass
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    float acc = 0.0f;
                    for (int k = -radius; k <= radius; ++k) {
                        int sy = std::clamp(y + k, 0, h - 1);
                        acc += tmp[sy * w + x] * kernel[k + radius];
                    }
                    src[y * w + x] = acc;
                }
            }

            // Build RGBA glow pixels (white with blurred alpha)
            std::vector<unsigned char> glow_pixels(w * h * 4);
            for (int i = 0; i < w * h; ++i) {
                unsigned char a = (unsigned char)std::clamp(src[i] * 255.0f, 0.0f, 255.0f);
                glow_pixels[i * 4 + 0] = 255;
                glow_pixels[i * 4 + 1] = 255;
                glow_pixels[i * 4 + 2] = 255;
                glow_pixels[i * 4 + 3] = a;
            }

            if (createTextureFromPixels(glow_pixels.data(), w, h, m_splashGlow)) {
                m_splashGlowDescriptorSet = ImGui_ImplVulkan_AddTexture(m_defaultSampler, m_splashGlow.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    }

    // 3. Load animated loading icons
    if (createTextureFromFile("assets/icon/eruption_v3_64.png", m_loadingIcons[0])) {
        m_loadingIconDS[0] = ImGui_ImplVulkan_AddTexture(m_defaultSampler, m_loadingIcons[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (createTextureFromFile("assets/icon/eruption_v3_64_2.png", m_loadingIcons[1])) {
        m_loadingIconDS[1] = ImGui_ImplVulkan_AddTexture(m_defaultSampler, m_loadingIcons[1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // 4. Create Splash Pipeline
    if (m_splashLogo.view != VK_NULL_HANDLE && m_splashSilhouette.view != VK_NULL_HANDLE) {
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        vkCreateDescriptorSetLayout(m_vulkan.device(), &layoutInfo, nullptr, &m_splashDescriptorSetLayout);

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(float) * 4;

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_splashDescriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        vkCreatePipelineLayout(m_vulkan.device(), &pipelineLayoutInfo, nullptr, &m_splashPipelineLayout);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_imguiPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_splashDescriptorSetLayout;
        if (vkAllocateDescriptorSets(m_vulkan.device(), &allocInfo, &m_splashDS) == VK_SUCCESS) {
            VkDescriptorImageInfo imageInfos[2] = {};
            imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos[0].imageView = m_splashLogo.view;
            imageInfos[0].sampler = m_defaultSampler;
            imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos[1].imageView = m_splashSilhouette.view;
            imageInfos[1].sampler = m_defaultSampler;

            VkWriteDescriptorSet writes[2] = {};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = m_splashDS;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &imageInfos[0];
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = m_splashDS;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &imageInfos[1];
            vkUpdateDescriptorSets(m_vulkan.device(), 2, writes, 0, nullptr);
        }

        auto vertCode = ShaderCompiler::loadSPIRV("postprocess/fullscreen.vert.spv");
        auto fragCode = ShaderCompiler::loadSPIRV("postprocess/splash.frag.spv");
        
        if (!vertCode.empty() && !fragCode.empty()) {
            auto createMod = [&](const std::vector<uint32_t>& code) {
                VkShaderModuleCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                ci.codeSize = code.size() * sizeof(uint32_t); ci.pCode = code.data();
                VkShaderModule mod = VK_NULL_HANDLE; 
                if (vkCreateShaderModule(m_vulkan.device(), &ci, nullptr, &mod) != VK_SUCCESS) return (VkShaderModule)VK_NULL_HANDLE;
                return mod;
            };
            VkShaderModule vMod = createMod(vertCode);
            VkShaderModule fMod = createMod(fragCode);

            if (vMod != VK_NULL_HANDLE && fMod != VK_NULL_HANDLE) {
                VkPipelineShaderStageCreateInfo stages[2] = {};
                stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vMod; stages[0].pName = "main";
                stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fMod; stages[1].pName = "main";

                VkPipelineColorBlendAttachmentState blend{};
                blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                blend.blendEnable = VK_TRUE;
                blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                blend.colorBlendOp = VK_BLEND_OP_ADD;
                blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                blend.alphaBlendOp = VK_BLEND_OP_ADD;

                PipelineBuilder builder;
                builder.setShaderStages({stages[0], stages[1]})
                       .setLayout(m_splashPipelineLayout)
                       .setColorAttachmentFormats({postColorFormat(m_vulkan.physicalDevice())}) // Render to HDR
                       .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                       .setPolygonMode(VK_POLYGON_MODE_FILL)
                       .setDepthState(false, false, VK_COMPARE_OP_ALWAYS)
                       .setBlendState({blend})
                       .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR});
                
                m_splashPipeline = builder.build(m_vulkan.device());
            }

            if (vMod != VK_NULL_HANDLE) vkDestroyShaderModule(m_vulkan.device(), vMod, nullptr);
            if (fMod != VK_NULL_HANDLE) vkDestroyShaderModule(m_vulkan.device(), fMod, nullptr);
        }
    }
}

void Engine::shutdownSplashLogo() {
    auto cleanup = [&](TextureResource& res, VkDescriptorSet& ds) {
        if (res.view != VK_NULL_HANDLE) vkDestroyImageView(m_vulkan.device(), res.view, nullptr);
        if (res.image != VK_NULL_HANDLE) vmaDestroyImage(m_vulkan.allocator(), res.image, res.alloc);
        res = {};
        ds = VK_NULL_HANDLE;
    };

    cleanup(m_splashLogo, m_splashLogoDescriptorSet);
    cleanup(m_splashSilhouette, m_splashSilhouetteDescriptorSet);
    cleanup(m_splashGlow, m_splashGlowDescriptorSet);
    cleanup(m_loadingIcons[0], m_loadingIconDS[0]);
    cleanup(m_loadingIcons[1], m_loadingIconDS[1]);

    if (m_splashPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_vulkan.device(), m_splashPipeline, nullptr);
    if (m_splashPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_vulkan.device(), m_splashPipelineLayout, nullptr);
    if (m_splashDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_vulkan.device(), m_splashDescriptorSetLayout, nullptr);
}

void Engine::renderLoadingUI(const std::string& mapName, const std::string& task, float progress) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 displaySize = io.DisplaySize;

    if (m_isInitialBoot) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        if (m_splashLogoDescriptorSet != VK_NULL_HANDLE) {
            float logoWidth = (float)m_splashLogo.width * 0.5f;
            float logoHeight = (float)m_splashLogo.height * 0.5f;
            // Move up by 0.25 of the logo height (which is 0.5 of the current 128px "padding" from center)
            ImVec2 center(displaySize.x * 0.5f, displaySize.y * 0.5f - logoHeight * 0.25f);
            ImVec2 p0(center.x - logoWidth * 0.5f, center.y - logoHeight * 0.5f);
            ImVec2 p1(center.x + logoWidth * 0.5f, center.y + logoHeight * 0.5f);

            ImVec2 logoCenter((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);

            // Two independent glows BEHIND the logo.
            // The blurred silhouette texture acts as a halo/contour around the logo.
            // Order: larger/bar glow (bottom), smaller/task glow (middle), logo (top).
            if (m_splashGlowDescriptorSet != VK_NULL_HANDLE) {
                // Smooth 60fps sine breathing, different periods so they feel alive.
                float taskBreath = 0.5f + 0.5f * std::sin(m_timer.elapsed() * 1.8f);
                float barBreath  = 0.5f + 0.5f * std::sin(m_timer.elapsed() * 2.5f);

                // Helper: draw a multi-layer glow for a smooth professional fadeout.
                auto drawLayeredGlow = [&](float progress, float breath, float minGlow, float maxGlow,
                                           float scaleRange, int baseAlpha, int peakAlpha,
                                           int r, int g, int b, int layers) {
                    float glowFactor = minGlow + (maxGlow - minGlow) * breath;
                    float startScale = 1.0f + 0.04f * glowFactor;
                    float endScale   = 1.0f + scaleRange * glowFactor;
                    int maxAlpha = baseAlpha + (int)((peakAlpha - baseAlpha) * glowFactor);

                    for (int i = 0; i < layers; ++i) {
                        float t = (float)i / (float)(layers - 1);
                        float scale = startScale + (endScale - startScale) * t;
                        float fade = std::pow(1.0f - t, 1.8f);
                        int alpha = (int)(maxAlpha * fade);
                        if (alpha <= 1) continue;

                        ImVec2 g0(logoCenter.x - (logoWidth  * 0.5f) * scale,
                                  logoCenter.y - (logoHeight * 0.5f) * scale);
                        ImVec2 g1(logoCenter.x + (logoWidth  * 0.5f) * scale,
                                  logoCenter.y + (logoHeight * 0.5f) * scale);
                        drawList->AddImage(m_splashGlowDescriptorSet, g0, g1, ImVec2(0,0), ImVec2(1,1), IM_COL32(r, g, b, alpha));
                    }
                };

                // 1) Larger bar glow: volcanic diffuse halo, grows with the bottom loading bar.
                drawLayeredGlow(m_splashBarProgress, barBreath, 0.02f, 1.2f, 0.35f, 3, 35,
                                255, 160, 60, 10);

                // 2) Smaller task glow: tighter inner halo, grows with the current boot task.
                drawLayeredGlow(m_splashTaskProgress, taskBreath, 0.03f, 1.0f, 0.12f, 4, 45,
                                255, 100, 20, 6);
            }

            // Draw main logo on top of the glows
            drawList->AddImage(m_splashLogoDescriptorSet, p0, p1);

            // Draw task text below the logo with 0.25 padding
            if (!task.empty()) {
                ImVec2 textSize = ImGui::CalcTextSize(task.c_str());
                float textY = p1.y + logoHeight * 0.25f; 
                drawList->AddText(ImVec2((displaySize.x - textSize.x) * 0.5f, textY), IM_COL32(200, 200, 200, 200), task.c_str());
            }
        }
    } else {
        // Modern Minimalist Van Halen inspired progress bar - Bottom Right
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        
        float width = 220.0f; 
        float height = 6.0f; 
        float margin = 40.0f;
        float rounding = 3.0f;
        
        ImVec2 pMax = ImVec2(displaySize.x - margin, displaySize.y - margin);
        ImVec2 pMin = ImVec2(pMax.x - width, pMax.y - height);
        
        // Van Halen "Frankenstrat" Colors (Modernized)
        ImU32 vhRed = IM_COL32(255, 60, 40, 255); // Brighter red/orange hue
        // Low contrast stripes: white with low alpha to blend with red
        ImU32 vhStripe = IM_COL32(255, 255, 255, 60); 
        ImU32 blackBg = IM_COL32(15, 15, 15, 180);
        
        // 1. Progress Bar Background (Rounded)
        drawList->AddRectFilled(pMin, pMax, blackBg, rounding);
        
        // 2. Filled Progress (Red, Rounded)
        float progressWidth = width * progress;
        if (progressWidth > 0.0f) {
            ImVec2 pFillMax = ImVec2(pMin.x + progressWidth, pMax.y);
            drawList->AddRectFilled(pMin, pFillMax, vhRed, rounding);
            
            // 3. Low Contrast Stripes (Van Halen style, clipped to progress)
            float dashStep = 8.0f;
            float dashWidth = 1.5f;
            drawList->PushClipRect(pMin, pFillMax, true);
            for (float x = pMin.x - 10.0f; x < pFillMax.x; x += dashStep) {
                drawList->AddLine(ImVec2(x, pMin.y - 2), ImVec2(x + 8.0f, pMax.y + 2), vhStripe, dashWidth);
            }
            drawList->PopClipRect();

            // Modern touch: slight top highlight
            drawList->AddLine(ImVec2(pMin.x + 1, pMin.y + 1), ImVec2(pFillMax.x - 1, pMin.y + 1), IM_COL32(255, 255, 255, 30), 1.0f);
        }
        
        // 4. Minimalist Text (Clean & Modern with Black Stroke)
        auto drawStrokedText = [&](ImVec2 pos, ImU32 col, const char* text) {
            drawList->AddText(ImVec2(pos.x - 1, pos.y - 1), IM_COL32(0, 0, 0, 255), text);
            drawList->AddText(ImVec2(pos.x + 1, pos.y - 1), IM_COL32(0, 0, 0, 255), text);
            drawList->AddText(ImVec2(pos.x - 1, pos.y + 1), IM_COL32(0, 0, 0, 255), text);
            drawList->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 255), text);
            drawList->AddText(pos, col, text);
        };

        std::string progressPct = std::to_string((int)(progress * 100.0f)) + "%";
        ImVec2 pctSize = ImGui::CalcTextSize(progressPct.c_str());
        
        // Position percentage at the end of the bar
        drawStrokedText(ImVec2(pMax.x - pctSize.x, pMin.y - pctSize.y - 5.0f), IM_COL32(200, 200, 200, 255), progressPct.c_str());
        
        // Task info and Map Name
        if (!task.empty()) {
            float yOffset = -18.0f;
            if (!mapName.empty()) {
                drawStrokedText(ImVec2(pMin.x, pMin.y + yOffset - 15.0f), vhRed, mapName.c_str());
                drawStrokedText(ImVec2(pMin.x, pMin.y + yOffset), IM_COL32(150, 150, 150, 255), task.c_str());
            } else {
                drawStrokedText(ImVec2(pMin.x, pMin.y + yOffset), IM_COL32(150, 150, 150, 255), task.c_str());
            }
        }
    }

    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), m_vulkan.currentCmdBuf());
}

} // namespace eruption
