// Captura de imagem do Engine: screenshot do alvo de post, screenshot do
// SWAPCHAIN (depois do ImGui compor por cima) e dump do minimapa. Saiu do
// Engine.cpp em 2026-09-04, na quebra do arquivo.
//
// A diferenca entre os dois caminhos de screenshot importa e ja' custou caro:
// takeScreenshot() le' m_postProcessor.outputImage() e por isso e' CEGO pra
// qualquer coisa desenhada pelo ImGui; scheduleScreenshotFromSwap() +
// finishPendingScreenshot() leem o swapchain DEPOIS do passe de UI. Uma
// investigacao inteira de "linha cinza na tela" nao reproduzia porque estava
// usando o primeiro.

#include "core/Engine.hpp"
#include "renderer/PostFormat.hpp"
#include "core/Logger.hpp"
#include "utils/ImageUtils.hpp"
#include <utility>
#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vector>
#include <cstring>
#include <cstdint>

namespace eruption {

static float halfToFloat(uint16_t h) {
    uint32_t s = (h >> 15) & 0x0001, e = (h >> 10) & 0x001f, m = h & 0x03ff;
    if (e == 0) { if (m == 0) return s ? -0.0f : 0.0f; else return (s ? -1.0f : 1.0f) * std::pow(2.0f, -14.0f) * (m / 1024.0f); }
    else if (e == 31) return m == 0 ? (s ? -INFINITY : INFINITY) : NAN;
    return (s ? -1.0f : 1.0f) * std::pow(2.0f, (float)e - 15.0f) * (1.0f + m / 1024.0f);
}

// Componentes do B10G11R11_UFLOAT_PACK32: float SEM sinal, 5 bits de expoente
// (vies 15) e M bits de mantissa - 6 para R/G (11 bits), 5 para B (10 bits).
static float ufloatToFloat(uint32_t bits, int mantissaBits) {
    const uint32_t mMask = (1u << mantissaBits) - 1u;
    const uint32_t e = (bits >> mantissaBits) & 0x1fu;
    const uint32_t m = bits & mMask;
    const float mf = static_cast<float>(m) / static_cast<float>(1u << mantissaBits);
    if (e == 0) return m == 0 ? 0.0f : std::pow(2.0f, -14.0f) * mf;
    if (e == 31) return m == 0 ? INFINITY : NAN;
    return std::pow(2.0f, static_cast<float>(e) - 15.0f) * (1.0f + mf);
}

// ERUPTION_TEST_DUMP_MINIMAP=<path> (debug): write the minimap texture itself
// to a PNG. The normal screenshot path captures the POST output, which is
// produced before ImGui draws, and the minimap is an ImGui foreground overlay --
// so the minimap can NEVER appear in a regular screenshot. Without this there is
// no way to visually verify the minimap headlessly.
void Engine::dumpMinimap(const std::string& filename) {
    if (m_minimapImage == VK_NULL_HANDLE) return;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    const uint32_t w = MINIMAP_RES, h = MINIMAP_RES;
    if (!m_vulkan.createBuffer(w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VMA_MEMORY_USAGE_GPU_TO_CPU, staging, stagingAlloc)) return;
    // The minimap image is left in SHADER_READ_ONLY_OPTIMAL, which is what
    // copyImageToBuffer expects.
    if (m_vulkan.copyImageToBuffer(m_minimapImage, staging, w, h)) {
        void* mapped = nullptr;
        vmaMapMemory(m_vulkan.allocator(), stagingAlloc, &mapped);
        std::vector<uint8_t> pixels(w * h * 4);
        memcpy(pixels.data(), mapped, pixels.size()); // R8G8B8A8_UNORM, direct copy
        vmaUnmapMemory(m_vulkan.allocator(), stagingAlloc);
        ImageUtils::writePNG(filename, w, h, 4, pixels.data());
        ERUPTION_LOG_WARN("[TEST] minimap dumped to %s", filename.c_str());
    }
    vmaDestroyBuffer(m_vulkan.allocator(), staging, stagingAlloc);
}

// Leitura da saida do POST (antes da UI e do upscale final) como RGB float,
// no formato real do alvo. takeScreenshot e o ERUPTION_TEST_ACCUM usam isto.
void Engine::readPostOutput(std::vector<float>& rgb, uint32_t& outW, uint32_t& outH) {
    // A captura le' a imagem de saida do POST, entao o tamanho tem que ser o
    // DELA, nao o do swapchain. Enquanto os dois coincidiam isso passava; quando
    // o post roda numa resolucao menor (render/post scale), a copia estourava os
    // limites da imagem e a screenshot saia PRETA:
    //   vkCmdCopyImageToBuffer: imageExtent.height + imageOffset.y (1024) deve
    //   ser <= altura do subrecurso da imagem (716)
    // Isso enganou dezesseis diagnosticos: a CENA sempre renderizou certo, so'
    // a captura falhava - e vkQueueSubmit nao retorna erro para isso.
    uint32_t w = m_postProcessor.width(), h = m_postProcessor.height();
    if (w == 0 || h == 0) { w = m_vulkan.swapExtent().width; h = m_vulkan.swapExtent().height; }
    VkBuffer staging; VmaAllocation stagingAlloc;
    // 8 B/px cobre RGBA16F; B10G11R11 usa 4 e sobra espaco (inofensivo).
    const VkFormat postFmt = postColorFormat(m_vulkan.physicalDevice());
    m_vulkan.createBuffer(w * h * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU, staging, stagingAlloc);

    // The post-processor leaves the output image in TRANSFER_SRC_OPTIMAL so it
    // can be copied to the swapchain. copyImageToBuffer assumes
    // SHADER_READ_ONLY_OPTIMAL, so issue the copy directly with the layout the
    // image is actually in.
    m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, m_postProcessor.outputImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging, 1, &region);
    });

    void* mapped; vmaMapMemory(m_vulkan.allocator(), stagingAlloc, &mapped);
    outW = w; outH = h;
    rgb.assign(size_t(w) * h * 3, 0.0f);
    // A captura tem que decodificar o formato REAL do alvo de post. Quando ele
    // virou B10G11R11 (metade da banda), continuar lendo half-float daria uma
    // imagem de lixo - e a screenshot e' a nossa unica verificacao visual.
    if (postFmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
        const uint32_t* src = static_cast<const uint32_t*>(mapped);
        for (uint32_t i = 0; i < w * h; ++i) {
            const uint32_t p = src[i];
            const float px3[3] = { ufloatToFloat(p & 0x7ffu, 6),
                                   ufloatToFloat((p >> 11) & 0x7ffu, 6),
                                   ufloatToFloat((p >> 22) & 0x3ffu, 5) };
            for (int c = 0; c < 3; ++c) rgb[size_t(i) * 3 + c] = px3[c];
        }
    } else {
        const uint16_t* src = static_cast<const uint16_t*>(mapped);
        for (uint32_t i = 0; i < w * h; ++i) for (int c = 0; c < 3; ++c) {
            rgb[size_t(i) * 3 + c] = halfToFloat(src[i * 4 + c]);
        }
    }
    vmaUnmapMemory(m_vulkan.allocator(), stagingAlloc); vmaDestroyBuffer(m_vulkan.allocator(), staging, stagingAlloc);
}

void Engine::takeScreenshot(const std::string& filename) {
    std::vector<float> rgb;
    uint32_t w = 0, h = 0;
    readPostOutput(rgb, w, h);
    std::vector<uint8_t> pixels(size_t(w) * h * 4, 255);
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i) for (int c = 0; c < 3; ++c)
        pixels[i * 4 + c] = static_cast<uint8_t>(glm::clamp(rgb[i * 3 + c] * 255.0f, 0.0f, 255.0f));
    ImageUtils::writePNG(filename, w, h, 4, pixels.data());
}

void Engine::scheduleScreenshotFromSwap(const std::string& filename) {
    m_screenshotPending = true;
    m_screenshotPendingPath = filename;
}

void Engine::finishPendingScreenshot() {
    if (!m_screenshotPending || m_screenshotStaging == VK_NULL_HANDLE) return;

    // Avoid a CPU stall: wait for the frame that recorded the copy instead of
    // idling the whole device. If it is not done yet, defer to the next frame.
    // No dump de TODOS os frames (ERUPTION_TEST_DUMP_FRAMES) a espera e'
    // bloqueante: diferir aqui faria o frame seguinte nem ser agendado, e a
    // sequencia sairia com buracos - justamente onde se procura o "jump".
    static const bool kDumpAll = std::getenv("ERUPTION_TEST_DUMP_FRAMES") != nullptr;
    VkFence fence = m_vulkan.frameFence(m_screenshotSubmitFrame);
    VkResult ready = vkWaitForFences(m_vulkan.device(), 1, &fence, VK_TRUE,
                                     kDumpAll ? UINT64_MAX : 0);
    if (ready != VK_SUCCESS) return;

    void* mapped = nullptr;
    vmaMapMemory(m_vulkan.allocator(), m_screenshotStagingAlloc, &mapped);
    std::vector<uint8_t> pixels(m_screenshotStagingW * m_screenshotStagingH * 4);
    memcpy(pixels.data(), mapped, pixels.size());
    vmaUnmapMemory(m_vulkan.allocator(), m_screenshotStagingAlloc);
    // O swapchain e' quase sempre B8G8R8A8: gravar os bytes crus como RGBA
    // trocava vermelho e azul (lava azul, terra roxa lilas nas capturas
    // SWAP_SHOT / --screenshot pos-ImGui). Corrige a ordem antes do PNG.
    if (m_vulkan.swapFormat() == VK_FORMAT_B8G8R8A8_UNORM || m_vulkan.swapFormat() == VK_FORMAT_B8G8R8A8_SRGB) {
        for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
    }
    vmaDestroyBuffer(m_vulkan.allocator(), m_screenshotStaging, m_screenshotStagingAlloc);
    m_screenshotStaging = VK_NULL_HANDLE;
    m_screenshotStagingAlloc = VK_NULL_HANDLE;

    ImageUtils::writePNG(m_screenshotPendingPath, m_screenshotStagingW, m_screenshotStagingH, 4, pixels.data());
    m_screenshotPending = false;
    m_screenshotPendingPath.clear();

    // No dump de todos os frames o run nao pode terminar no primeiro PNG.
    if (m_enableAutoExit && m_abCaptureStep == 0 && !kDumpAll) m_running = false;
}


} // namespace eruption
