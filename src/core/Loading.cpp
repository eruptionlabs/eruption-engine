// Tela de carregamento do Engine: splash com logo/silhueta/glow, barra de
// progresso, e o passe que apresenta tudo isso ENQUANTO o mapa carrega (fora
// do laco normal de frame, porque durante a carga o laco esta' bloqueado).
// Saiu do Engine.cpp em 2026-09-04, na quebra do arquivo.

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "utils/ImageUtils.hpp"
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "core/LoadPump.hpp"
#include "renderer/PostFormat.hpp"
#include "core/LoadingScreen.hpp"
#include <stb_image.h>
#include <cmath>
#include <string>
#include <vk_mem_alloc.h>
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace eruption {

// Margem (em texels da textura do halo, meia resolucao do logo) em volta da
// silhueta, para o desfoque largo caber inteiro.
static constexpr int kSplashGlowPad = 64;

// O splash (logo + halo) segue na tela durante a primeira carga de mapa,
// limpando o fundo a cada quadro. Antes, a carga desenhava so' a barra por
// cima de uma imagem velha da swapchain (texto da etapa anterior, halo
// parado). Desliga no "Finalizing" (0.95) da primeira carga ou no primeiro
// quadro normal (editor aberto sem mapa).
static bool s_bootSplash = true;

// Relogio do halo. m_timer so' anda no laco de quadro (tick), parado durante
// o boot, entao o pulso usa o relogio de parede desde initSplashLogo.
static std::chrono::steady_clock::time_point s_splashStart;

void Engine::onPipelineCompiled(void* engine) {
    static_cast<Engine*>(engine)->showShaderProgress();
}

void Engine::showShaderProgress() {
    ++m_shadersCompiled;
    // A própria tela pode criar pipeline; e pipeline criada no meio de uma
    // gravação (immediateSubmit, frame) não pode abrir outra tela ali.
    if (m_drawingShaderProgress || m_vulkan.busyRecording()) return;
    // ERUPTION_TEST_NO_SHADER_PROGRESS=1 (debug): conta mas não redesenha.
    static const bool kOff = std::getenv("ERUPTION_TEST_NO_SHADER_PROGRESS") != nullptr;
    if (kOff) return;
    const auto now = std::chrono::steady_clock::now();
    // Com o logo na tela, 30 Hz: o halo pulsa num ciclo de 1 s. O intervalo
    // nunca fica abaixo de 4x o custo do ultimo redesenho: onde apresentar e'
    // caro (xvfb, ~0,3 s por quadro) a tela gastava mais que a compilacao.
    static std::chrono::steady_clock::duration s_lastCost{};
    const auto interval = std::max<std::chrono::steady_clock::duration>(
        std::chrono::milliseconds((m_isInitialBoot || s_bootSplash) ? 33 : 200), s_lastCost * 4);
    if (now - m_lastShaderProgress < interval) return;
    m_lastShaderProgress = now;
    // O total não é conhecido: a barra avança cada vez menos, sem passar da
    // próxima etapa.
    const float extra = 0.12f * (1.0f - std::exp(-static_cast<float>(m_shadersCompiled) / 40.0f));
    m_drawingShaderProgress = true;
    presentLoadingScreen(m_loadingMap, m_loadingTask + " - compiling shaders (" + std::to_string(m_shadersCompiled) + ")",
                         m_loadingProgress + extra, m_loadingClear);
    m_drawingShaderProgress = false;
    s_lastCost = std::chrono::steady_clock::now() - now;
}

void Engine::presentLoadingScreen(const std::string& mapName, const std::string& task, float progress, bool clear) {
    if (!m_drawingShaderProgress) {
        m_loadingMap = mapName;
        m_loadingTask = task;
        m_loadingProgress = progress;
        m_loadingClear = clear;
        m_shadersCompiled = 0;
    }
    float clampedProgress = std::clamp(progress, 0.0f, 1.0f);

    // Smooth followers of the progress.
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

    const bool splash = m_isInitialBoot || s_bootSplash;
    if (clear || splash) {
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    } else {
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    }

    m_vulkan.cmdImageBarrier(cmd, m_vulkan.swapImage(imageIndex), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    // Fundo do projeto (bloco "loading"): toda tela de carga limpa nele,
    // inclusive a de troca de mapa, que antes desenhava sobre o ultimo quadro.
    if (const LoadingScreenStyle* style = projectLoadingStyle(); style && style->hasBackground) {
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.clearValue.color = {{style->background[0], style->background[1], style->background[2], 1.0f}};
    }
    m_vulkan.cmdBeginRendering(cmd, {colorAttachment}, nullptr, nullptr, m_vulkan.swapExtent());

    renderLoadingUI(mapName, task, progress);

    m_vulkan.cmdEndRendering(cmd);
    m_vulkan.cmdImageBarrier(cmd, m_vulkan.swapImage(imageIndex), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);

    m_vulkan.endFrame();

    if (!m_isInitialBoot && clampedProgress >= 0.95f) s_bootSplash = false;
}

// ------------------------------------------------------------------
// Background loading helpers (frame CB upload)
// ------------------------------------------------------------------

void Engine::setGameLoadProgress(float done, float total) {
    total = std::max(total, 0.0f);
    done = std::clamp(done, 0.0f, total);
    const float before = m_gameLoadTotal > 0.0f ? m_gameLoadDone / m_gameLoadTotal : 0.0f;
    m_gameLoadDone = std::max(m_gameLoadDone, done);
    m_gameLoadTotal = std::max(total, m_gameLoadDone);
    // Um aviso que avancou e' um passo: o ritmo (segundos e fracao por aviso,
    // medias moveis) vem dos avisos, nao de 1/total. O jogo que manda fracao
    // (feito 0.03 de total 1) tinha um "passo" do tamanho da fila inteira.
    const float gained = m_gameLoadTotal > 0.0f ? m_gameLoadDone / m_gameLoadTotal - before : 0.0f;
    if (gained > 1e-5f && m_gameStepAge > 0.0f) {
        m_gameStepSecs = std::clamp(0.5f * m_gameStepSecs + 0.5f * m_gameStepAge, 0.01f, 10.0f);
        m_gameStepFrac = m_gameStepFrac < 0.0f ? gained : 0.5f * m_gameStepFrac + 0.5f * gained;
        m_gameStepAge = 0.0f;
    }
}

void Engine::holdLoading(bool on, float maxSeconds) {
    if (!on && m_loadingHeld) m_loadingFinishing = true; // o release mostra 100% num quadro
    // Do zero: a conta da espera (motor + jogo) parte de onde a barra estava
    // e o max dela nunca deixa voltar; partir de 0.95 pulava a parte do jogo.
    if (on && !m_loadingHeld) m_loadingHeldProgress = 0.0f;
    m_loadingHeld = on;
    m_loadingHeldLeft = on ? std::max(maxSeconds, 0.0f) : 0.0f;
}

// Quadro com a carga segura pelo script: a mesma tela do fim da carga,
// barra subindo devagar sem chegar a 100% (so' o release termina).
void Engine::presentHeldLoading() {
    const float dt = std::min(m_timer.realDeltaTime(), 0.25f);
    m_loadingHeldLeft -= dt;
    if (m_loadingHeld && m_loadingHeldLeft <= 0.0f) {
        // Script que esqueceu o release (ou parou com erro) nao prende o jogo.
        ERUPTION_LOG_WARN("[LOADING] loading.hold sem release: soltando pelo tempo maximo");
        m_loadingHeld = false;
        render();
        return;
    }
    // A barra segue o trabalho pendente do motor (a mesma conta do
    // loading.progress) e a fila do jogo (loading.game_progress), cada um na
    // sua parte (game_share do bloco "loading"), sem chegar a 100%: so' o
    // release termina, com um quadro a 100% antes da cena.
    float done = 0.0f, total = 1.0f;
    bool ready = false;
    loadProgress(done, total, ready);
    if (m_loadingFinishing) {
        m_loadingHeldProgress = 1.0f;
    } else {
        // Entre dois avisos do jogo a barra anda no ritmo medido dos avisos
        // anteriores e vai freando, com um piso a 30% desse ritmo; os dois
        // param 2 passos a frente do informado. O max a segura quando o
        // proximo aviso chega atras dela. O piso antigo (25%/s do que falta,
        // sem teto) chegava a 99% em 40% de uma fila linear e parava ~20 s.
        // Simulado: fila linear de 30 s acompanha o informado a 0,3%; passos
        // irregulares, ate' 17% de desvio.
        m_gameStepAge += dt;
        float game = 0.0f;
        if (m_gameLoadTotal > 0.0f) {
            const float reported = m_gameLoadDone / m_gameLoadTotal;
            const float step = m_gameStepFrac > 0.0f ? m_gameStepFrac : std::min(1.0f / m_gameLoadTotal, 0.05f);
            const float ceiling = std::min(reported + 2.0f * step, 1.0f);
            const float walk = 2.0f * (1.0f - std::exp(-m_gameStepAge / (2.0f * m_gameStepSecs)));
            const float floorWalk = std::min(m_gameLoadShown + 0.3f * step / m_gameStepSecs * dt, ceiling);
            game = std::min(std::max(reported + step * walk, floorWalk), ceiling);
        }
        m_gameLoadShown = std::max(m_gameLoadShown, game);
        const LoadingScreenStyle* style = projectLoadingStyle();
        const float share = style ? style->gameShare : 0.0f;
        const float engine = total > 0.0f ? done / total : 1.0f;
        const float bar = (1.0f - share) * engine + share * m_gameLoadShown;
        m_loadingHeldProgress = std::max(m_loadingHeldProgress, std::min(bar, 0.99f));
    }
    m_presentingHeld = true;
    presentLoadingScreen(m_currentMapName, "Finalizing", m_loadingHeldProgress, true);
    m_presentingHeld = false;
    if (m_loadingFinishing) {
        // A fila do jogo recomeca na proxima carga.
        m_loadingFinishing = false;
        m_gameLoadDone = m_gameLoadTotal = m_gameLoadShown = m_gameStepAge = 0.0f;
        m_gameStepSecs = 2.0f;
        m_gameStepFrac = -1.0f;
    }
}

void Engine::renderLoadingProgress() {
    m_firstLoadOver = true; // a primeira carga (e a espera dela) acabou
    s_bootSplash = false; // ja' houve quadro normal: o boot acabou
    // Barra enquanto um mapa é lido em segundo plano (troca pelo seletor).
    // Nuvens do clima semeando NAO contam: o jogo troca o clima no meio da
    // partida (uma skill de tempestade) e a barra "Carregando" aparecia no
    // centro da tela com o jogo rodando. Cada nuvem custa 0,1-0,6 ms de CPU
    // (sem espera de GPU) e a fila anda com 2 ms por quadro.
    const bool hasActiveLoad = m_warpJobActive;
    if (!hasActiveLoad) return;

    const float progress = 0.4f;
    const std::string task = "Loading map data";
    std::string mapName = m_warpJobMap;

    // Smooth follower for the bottom loading bar progress shown by the logo glow.
    if (progress > m_splashBarProgress) {
        m_splashBarProgress += (progress - m_splashBarProgress) * 0.5f;
    }

    // Projeto com tela de carga propria: a mesma barra, sem fundo (o jogo
    // continua desenhado atras durante a troca em segundo plano).
    if (const LoadingScreenStyle* style = projectLoadingStyle()) {
        drawProjectLoadingBar(ImGui::GetForegroundDrawList(), ImGui::GetIO().DisplaySize, *style, mapName, task, progress);
        return;
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
    s_splashStart = std::chrono::steady_clock::now();
    // Enquanto o logo estiver na tela, espera longa redesenha (o halo segue pulsando).
    setLoadPump([this] {
        if (!(m_isInitialBoot || s_bootSplash)) return;
        presentLoadingScreen(m_loadingMap, m_loadingTask, m_loadingProgress, m_loadingClear);
    });
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

        // 2b. Halo do logo. O logo aparece a 0.5x, entao a silhueta desce a
        // meia resolucao (media 2x2, ja' com borda suave) e ganha margem: o
        // halo morre dentro da textura, sem corte reto na borda do quad.
        // Duas larguras de desfoque: uma justa (calor colado ao logo) e uma
        // larga (o halo). A cor vai pre-assada, laranja claro perto do logo e
        // vermelho na ponta, e o alpha leva dither para nao bandear.
        // O desenho so' anima a intensidade (renderLoadingUI).
        {
            const int gw = w / 2 + 2 * kSplashGlowPad;
            const int gh = h / 2 + 2 * kSplashGlowPad;
            std::vector<float> mask(static_cast<size_t>(gw) * gh, 0.0f);
            for (int y = 0; y < h / 2; ++y) {
                for (int x = 0; x < w / 2; ++x) {
                    float acc = 0.0f;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx)
                            acc += silhouette_pixels[(static_cast<size_t>(2 * y + dy) * w + (2 * x + dx)) * 4 + 3];
                    mask[static_cast<size_t>(y + kSplashGlowPad) * gw + (x + kSplashGlowPad)] = acc / (4.0f * 255.0f);
                }
            }

            auto blur = [&](const std::vector<float>& in, float sigma) {
                const int radius = static_cast<int>(std::ceil(sigma * 3.0f));
                std::vector<float> kernel(2 * radius + 1);
                float sum = 0.0f;
                for (int i = -radius; i <= radius; ++i) {
                    kernel[i + radius] = std::exp(-static_cast<float>(i * i) / (2.0f * sigma * sigma));
                    sum += kernel[i + radius];
                }
                for (float& v : kernel) v /= sum;
                // Fora da textura e' zero (a margem garante que nada se perde).
                std::vector<float> tmp(in.size()), out(in.size());
                for (int y = 0; y < gh; ++y)
                    for (int x = 0; x < gw; ++x) {
                        float acc = 0.0f;
                        for (int k = std::max(-radius, -x); k <= std::min(radius, gw - 1 - x); ++k)
                            acc += in[static_cast<size_t>(y) * gw + (x + k)] * kernel[k + radius];
                        tmp[static_cast<size_t>(y) * gw + x] = acc;
                    }
                for (int y = 0; y < gh; ++y)
                    for (int x = 0; x < gw; ++x) {
                        float acc = 0.0f;
                        for (int k = std::max(-radius, -y); k <= std::min(radius, gh - 1 - y); ++k)
                            acc += tmp[static_cast<size_t>(y + k) * gw + x] * kernel[k + radius];
                        out[static_cast<size_t>(y) * gw + x] = acc;
                    }
                return out;
            };
            const std::vector<float> tight = blur(mask, 5.0f);
            const std::vector<float> wide = blur(mask, 18.0f);

            std::vector<unsigned char> glow_pixels(static_cast<size_t>(gw) * gh * 4);
            for (int i = 0; i < gw * gh; ++i) {
                const float g = std::min(1.0f, 0.5f * tight[i] + 1.1f * wide[i]);
                // Calor: 0 na ponta do halo, 1 colado no logo.
                const float heat = std::clamp(g * 1.6f, 0.0f, 1.0f);
                // Dither triangular de +-1 nivel (hash inteiro, estavel).
                uint32_t hsh = static_cast<uint32_t>(i) * 2654435761u;
                hsh ^= hsh >> 15; hsh *= 2246822519u; hsh ^= hsh >> 13;
                const float dither = (static_cast<float>(hsh & 0xFFu) + static_cast<float>((hsh >> 8) & 0xFFu)) / 255.0f - 1.0f;
                // Fora do halo fica zero exato: o dither nao suja o resto do quad.
                const float a = g < 1.0f / 255.0f ? 0.0f : std::clamp(g * 255.0f + dither, 0.0f, 255.0f);
                glow_pixels[i * 4 + 0] = 255;
                glow_pixels[i * 4 + 1] = static_cast<unsigned char>(70.0f + 100.0f * heat);
                glow_pixels[i * 4 + 2] = static_cast<unsigned char>(15.0f + 45.0f * heat * heat);
                glow_pixels[i * 4 + 3] = static_cast<unsigned char>(a + 0.5f);
            }

            if (createTextureFromPixels(glow_pixels.data(), gw, gh, m_splashGlow)) {
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

// A escala do framebuffer da UI sai do swapchain DE VERDADE. No Android o
// shim supoe a escala de tela do preset (display_scale), mas o swapchain so'
// encolhe quando o quadro do jogo processa a troca - na carga ele ainda esta'
// cheio, e a tela de carregamento saia desenhada a 0,5/0,75 no canto ("o app
// abre pequeno"). No desktop o swapchain e' o framebuffer: nada muda.
void Engine::syncUiFramebufferScale() {
    ImGuiIO& io = ImGui::GetIO();
    const VkExtent2D e = m_vulkan.swapExtent();
    if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f && e.width && e.height)
        io.DisplayFramebufferScale = ImVec2(float(e.width) / io.DisplaySize.x, float(e.height) / io.DisplaySize.y);
}

void Engine::renderLoadingUI(const std::string& mapName, const std::string& task, float progress) {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    syncUiFramebufferScale();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 displaySize = io.DisplaySize;

    // Projeto com tela de carga propria: so' ela, do boot ao fim, sem o logo.
    if (const LoadingScreenStyle* style = projectLoadingStyle()) {
        // Boot e primeiro mapa numa barra so', que nunca volta: o boot ocupa
        // os primeiros 30% e o mapa o resto. Separadas, a barra ia a 80% no
        // boot e caia para 10% no inicio do mapa.
        // Acaba no primeiro quadro normal: a espera do loading.hold depois do
        // mapa ainda e' a primeira carga (o script ve' a mesma conta).
        static float s_bootShown = 0.0f;
        float shown = std::clamp(progress, 0.0f, 1.0f);
        if (m_presentingHeld) {
            // A espera ja' manda a barra pronta (motor + jogo, presentHeldLoading).
            // Uma carga de mapa com o hold ja' ligado (hold no topo do script,
            // lido no boot) passa pelo outro ramo, como qualquer carga.
        } else {
            if (!m_firstLoadOver) {
                shown = m_isInitialBoot ? 0.3f * shown : 0.3f + 0.7f * shown;
                shown = std::max(shown, s_bootShown);
                s_bootShown = shown;
            }
            // O fim da barra e' da fila do jogo: o motor anda so' no resto.
            shown *= 1.0f - style->gameShare;
        }
        drawProjectLoadingBar(ImGui::GetForegroundDrawList(), displaySize, *style, mapName, task, shown);
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), m_vulkan.currentCmdBuf());
        return;
    }

    if (m_isInitialBoot || s_bootSplash) {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        if (m_splashLogoDescriptorSet != VK_NULL_HANDLE) {
            float logoWidth = (float)m_splashLogo.width * 0.5f;
            float logoHeight = (float)m_splashLogo.height * 0.5f;
            // Move up by 0.25 of the logo height (which is 0.5 of the current 128px "padding" from center)
            ImVec2 center(displaySize.x * 0.5f, displaySize.y * 0.5f - logoHeight * 0.25f);
            ImVec2 p0(center.x - logoWidth * 0.5f, center.y - logoHeight * 0.5f);
            ImVec2 p1(center.x + logoWidth * 0.5f, center.y + logoHeight * 0.5f);

            ImVec2 logoCenter((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);

            // Halo atras do logo: um quad so', com a textura assada em
            // initSplashLogo. Pulsa num ciclo de 1 s de relogio de parede:
            // quase apagado no vale, suave no pico.
            if (m_splashGlowDescriptorSet != VK_NULL_HANDLE) {
                constexpr float kTwoPi = 6.28318530718f;
                constexpr float kPeriod = 2.0f; // segundos por pulso
                constexpr float kBase = 0.05f;  // vale
                constexpr float kAmp = 0.60f;   // pico = kBase + kAmp
                const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_splashStart).count();
                const float pulse = 0.5f + 0.5f * std::sin(kTwoPi * t / kPeriod);
                const float intensity = kBase + kAmp * pulse;
                // Texel do halo = 2 texels do logo; a margem cresce um pouco no pico.
                const float texel = logoWidth / (static_cast<float>(m_splashGlow.width) - 2.0f * kSplashGlowPad);
                const float halfW = 0.5f * static_cast<float>(m_splashGlow.width) * texel * (1.0f + 0.03f * pulse);
                const float halfH = 0.5f * static_cast<float>(m_splashGlow.height) * texel * (1.0f + 0.03f * pulse);
                drawList->AddImage(m_splashGlowDescriptorSet,
                                   ImVec2(logoCenter.x - halfW, logoCenter.y - halfH),
                                   ImVec2(logoCenter.x + halfW, logoCenter.y + halfH),
                                   ImVec2(0, 0), ImVec2(1, 1),
                                   IM_COL32(255, 255, 255, static_cast<int>(intensity * 255.0f + 0.5f)));
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
    }
    // Barra do canto: em toda carga de mapa, inclusive a primeira (com o logo na tela).
    if (!m_isInitialBoot) {
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
