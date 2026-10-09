#include "renderer/VulkanContext.hpp"
#include <cstdio>
#include <csignal>
#include <chrono>
#include "core/Logger.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace eruption {

bool VulkanContext::init(GLFWwindow* window) {
    m_window = window;
    if (!createInstance()) return false;
    if (!setupDebugMessenger()) return false;
    if (!createSurface(window)) return false;
    if (!pickPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;
    if (!createSwapchain()) return false;
    if (!createImageViews()) return false;
    if (!createAllocator()) return false;
    if (!createSyncObjects()) return false;
    if (!createCommandPools()) return false;
    if (!createTimestampPools()) return false;
    if (!createPipelineStatsPools()) return false;
    createPipelineCache();

    ERUPTION_LOG_INFO("Vulkan context initialized successfully");
    return true;
}

VkPipelineCache VulkanContext::s_pipelineCache = VK_NULL_HANDLE;

static const char* kPipelineCachePath = ".cache/eruption/pipeline_cache.bin";

void VulkanContext::createPipelineCache() {
    std::vector<char> initial;
    std::ifstream in(kPipelineCachePath, std::ios::binary | std::ios::ate);
    if (in.is_open()) {
        const auto size = static_cast<size_t>(in.tellg());
        if (size > 32) { // VkPipelineCacheHeaderVersionOne is 32 bytes
            initial.resize(size);
            in.seekg(0);
            in.read(initial.data(), size);
        }
    }
    // Only feed data written by this exact GPU/driver back to the driver.
    if (!initial.empty()) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        uint32_t vendorID = 0, deviceID = 0;
        std::memcpy(&vendorID, initial.data() + 8, sizeof(uint32_t));
        std::memcpy(&deviceID, initial.data() + 12, sizeof(uint32_t));
        if (vendorID != props.vendorID || deviceID != props.deviceID ||
            std::memcmp(initial.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
            ERUPTION_LOG_INFO("Pipeline cache on disk is for another GPU/driver, starting fresh");
            initial.clear();
        }
    }

    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = initial.size();
    info.pInitialData = initial.empty() ? nullptr : initial.data();
    if (vkCreatePipelineCache(m_device, &info, nullptr, &m_pipelineCache) != VK_SUCCESS &&
        !initial.empty()) {
        // Corrupt blob: retry empty.
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        vkCreatePipelineCache(m_device, &info, nullptr, &m_pipelineCache);
    }
    s_pipelineCache = m_pipelineCache;
    ERUPTION_LOG_INFO("Pipeline cache ready (%zu bytes warm)", initial.size());
}

void VulkanContext::destroyPipelineCache() {
    if (m_pipelineCache == VK_NULL_HANDLE) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, nullptr) == VK_SUCCESS && size > 0) {
        std::vector<char> blob(size);
        if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, blob.data()) == VK_SUCCESS) {
            std::error_code ec;
            std::filesystem::create_directories(".cache/eruption", ec);
            std::ofstream out(kPipelineCachePath, std::ios::binary | std::ios::trunc);
            if (out.is_open()) out.write(blob.data(), static_cast<std::streamsize>(size));
        }
    }
    vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
    m_pipelineCache = VK_NULL_HANDLE;
    s_pipelineCache = VK_NULL_HANDLE;
}

void VulkanContext::shutdown() {
    waitIdle();

    // Execute any deferred per-frame cleanups that were scheduled but never
    // reached the next frame (e.g. staging buffers uploaded on the last frame).
    for (auto& queue : m_frameCleanupQueues) {
        for (auto& fn : queue) {
            fn();
        }
        queue.clear();
    }

    if (m_transferPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_transferPool, nullptr);
        m_transferPool = VK_NULL_HANDLE;
    }

    destroyTimestampPools();
    destroyPipelineStatsPools();

    for (auto& pool : m_commandPools) {
        if (pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, pool, nullptr);
        }
    }
    m_commandPools.clear();

    for (auto& fence : m_frameFences) {
        if (fence != VK_NULL_HANDLE) {
            vkDestroyFence(m_device, fence, nullptr);
        }
    }
    m_frameFences.clear();

    for (auto& sem : m_imageAvailable) {
        if (sem != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, sem, nullptr);
        }
    }
    m_imageAvailable.clear();

    for (auto& sem : m_renderFinished) {
        if (sem != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device, sem, nullptr);
        }
    }
    m_renderFinished.clear();

    cleanupSwapchain();

    if (m_allocator != VK_NULL_HANDLE) {
        // CENSO DE VAZAMENTO antes de destruir o alocador. Em build de debug o
        // VMA aborta se sobrar alocacao ("Some allocations were not freed
        // before destruction of this memory block!"); em release NDEBUG cala
        // o assert e o vazamento some sem deixar rastro. Este log e' o rastro:
        // contagem e bytes do que chegou vivo ate' aqui, para o vazamento ser
        // visivel no binario que a bancada de fato roda.
        VmaTotalStatistics st{};
        vmaCalculateStatistics(m_allocator, &st);
        const uint32_t leaked = st.total.statistics.allocationCount;
        if (leaked > 0) {
            ERUPTION_LOG_WARN("[VMA] %u alocacao(oes) vivas no shutdown (%.1f KB) - "
                              "vazamento; em build de debug isto aborta",
                              leaked, st.total.statistics.allocationBytes / 1024.0);
            // ERUPTION_TEST_VMA_DUMP=<arquivo>: inventario JSON do VMA com cada
            // bloco e alocacao viva (tamanho, tipo, nome se houver) - e' como
            // se acha QUEM vazou.
            if (const char* dump = std::getenv("ERUPTION_TEST_VMA_DUMP")) dumpVmaStats(dump);
        } else {
            ERUPTION_LOG_WARN("[VMA] shutdown limpo: 0 alocacoes vivas");
        }
        vmaDestroyAllocator(m_allocator);
        m_allocator = VK_NULL_HANDLE;
    }

    destroyPipelineCache();

    if (m_device != VK_NULL_HANDLE) {
        vkc::shutdown();
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }

    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }

    if (m_debugMessenger != VK_NULL_HANDLE) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func) {
            func(m_instance, m_debugMessenger, nullptr);
        }
        m_debugMessenger = VK_NULL_HANDLE;
    }

    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    ERUPTION_LOG_INFO("Vulkan context shut down");
}

bool VulkanContext::beginFrame() {
    if (m_deviceLost) return false;

    // ERUPTION_DEBUG_PRESENT=1: mede onde o frame realmente para. Existe um
    // custo fixo de ~6,5 ms por frame que NAO depende de cena, clima, preset
    // nem resolucao (medido: 6,53 ms tanto no preset high com 2,07 ms de GPU
    // quanto no low com 0,95 ms). Sem separar fence/acquire/present nao da'
    // para saber se e' sincronizacao, apresentacao ou trabalho de CPU.
    static const bool kDbgPresent = std::getenv("ERUPTION_DEBUG_PRESENT") != nullptr;
    using Clk = std::chrono::steady_clock;
    auto t0 = kDbgPresent ? Clk::now() : Clk::time_point{};
    vkWaitForFences(m_device, 1, &m_frameFences[m_currentFrame], VK_TRUE, UINT64_MAX);
    vkc::beginFrame(m_currentFrame);
    for (auto& hook : m_frameBeginHooks) hook(m_currentFrame);
    auto t1 = kDbgPresent ? Clk::now() : Clk::time_point{};

    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                                            m_imageAvailable[m_currentFrame],
                                            VK_NULL_HANDLE, &m_imageIndex);
    if (kDbgPresent) {
        auto t2 = Clk::now();
        m_dbgFenceMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        m_dbgAcquireMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
    }

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return recreateSwapchain();
    } else if (result == VK_ERROR_DEVICE_LOST) {
        ERUPTION_LOG_ERROR("Vulkan device lost");
        m_deviceLost = true;
        return false;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        ERUPTION_LOG_ERROR("Failed to acquire swap chain image");
        return false;
    }

    // Read back timestamp query results from the previous use of this frame slot.
    // The fence wait above already guarantees the slot's prior submission
    // completed, so no vkDeviceWaitIdle is needed (a full-device wait here used
    // to serialize CPU/GPU every frame). WITH_AVAILABILITY keeps unwritten
    // queries from failing the whole read: the buffer is (value, availability)
    // pairs, stride = 2 uint64 per query.
    if (!m_timestampPools.empty() && m_timestampPools[m_currentFrame] != VK_NULL_HANDLE) {
        m_timestampResults[m_currentFrame].assign(TIMESTAMP_QUERY_COUNT * 2, 0);
        VkResult qr = vkGetQueryPoolResults(m_device, m_timestampPools[m_currentFrame], 0, TIMESTAMP_QUERY_COUNT,
                                            sizeof(uint64_t) * TIMESTAMP_QUERY_COUNT * 2, m_timestampResults[m_currentFrame].data(),
                                            sizeof(uint64_t) * 2, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (qr != VK_SUCCESS && qr != VK_NOT_READY) {
            ERUPTION_LOG_WARN("vkGetQueryPoolResults failed: %d", qr);
            std::fill(m_timestampResults[m_currentFrame].begin(), m_timestampResults[m_currentFrame].end(), 0);
        }
    }

    // Pipeline statistics do mesmo slot. Sem WITH_AVAILABILITY: e' uma
    // consulta so' e a fence acima ja' garantiu que o frame terminou; se ela
    // nao tiver sido escrita (passe pulado), VK_NOT_READY deixa os zeros.
    if (m_pipeStatsSupported && !m_pipeStatsPools.empty() &&
        m_pipeStatsPools[m_currentFrame] != VK_NULL_HANDLE) {
        auto& dst = m_pipeStatsResults[m_currentFrame];
        dst.assign(PIPE_STATS_COUNT, 0);
        vkGetQueryPoolResults(m_device, m_pipeStatsPools[m_currentFrame], 0, 1,
                              sizeof(uint64_t) * PIPE_STATS_COUNT, dst.data(),
                              sizeof(uint64_t) * PIPE_STATS_COUNT,
                              VK_QUERY_RESULT_64_BIT);
    }

    // Cleanup deferred resources for this frame slot
    for (auto& fn : m_frameCleanupQueues[m_currentFrame]) {
        fn();
    }
    m_frameCleanupQueues[m_currentFrame].clear();

    vkResetFences(m_device, 1, &m_frameFences[m_currentFrame]);
    vkResetCommandPool(m_device, m_commandPools[m_currentFrame], 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &beginInfo));

    if (!m_timestampPools.empty() && m_timestampPools[m_currentFrame] != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(m_commandBuffers[m_currentFrame], m_timestampPools[m_currentFrame], 0, TIMESTAMP_QUERY_COUNT);
    }
    // A consulta de estatistica precisa ser resetada antes de cada uso, no
    // mesmo lugar do reset dos timestamps (inicio da gravacao do frame).
    if (m_pipeStatsSupported && !m_pipeStatsPools.empty() &&
        m_pipeStatsPools[m_currentFrame] != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(m_commandBuffers[m_currentFrame], m_pipeStatsPools[m_currentFrame], 0, 1);
    }

    m_isFrameRecording = true;
    return true;
}

bool VulkanContext::endFrame() {
    m_isFrameRecording = false;
    VK_CHECK(vkEndCommandBuffer(m_commandBuffers[m_currentFrame]));

    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &m_imageAvailable[m_currentFrame];
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffers[m_currentFrame];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &m_renderFinished[m_imageIndex]; // por imagem, nao por frame

    VkResult submitResult = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_frameFences[m_currentFrame]);
    if (submitResult == VK_ERROR_DEVICE_LOST) {
        m_deviceLost = true;
        return false;
    } else if (submitResult != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("vkQueueSubmit failed: %d", submitResult);
        return false;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &m_renderFinished[m_imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_swapchain;
    presentInfo.pImageIndices = &m_imageIndex;

    static const bool kDbgPresent2 = std::getenv("ERUPTION_DEBUG_PRESENT") != nullptr;
    // Medido SEMPRE (duas leituras de relogio por frame, custo desprezivel):
    // sob xvfb o present por software leva ~7,4 ms e domina o frame, criando um
    // teto artificial de ~135 FPS. Sem expor esse numero, nenhum FPS colhido
    // headless pode ser interpretado.
    const auto tp0 = std::chrono::steady_clock::now();
    VkResult result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
    m_lastPresentMs = static_cast<float>(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp0).count());
    if (kDbgPresent2) {
        m_dbgPresentMs += m_lastPresentMs;
        if (++m_dbgFrames % 300 == 0) {
            ERUPTION_LOG_WARN("[PRESENT] por frame: fence=%.2f ms acquire=%.2f ms present=%.2f ms (soma=%.2f)",
                              m_dbgFenceMs / 300.0, m_dbgAcquireMs / 300.0, m_dbgPresentMs / 300.0,
                              (m_dbgFenceMs + m_dbgAcquireMs + m_dbgPresentMs) / 300.0);
            m_dbgFenceMs = m_dbgAcquireMs = m_dbgPresentMs = 0.0;
        }
    }

    if (result == VK_ERROR_DEVICE_LOST) {
        m_deviceLost = true;
        return false;
    } else if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebufferResized) {
        m_framebufferResized = false;
        return recreateSwapchain();
    } else if (result != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("Failed to present swap chain image");
        return false;
    }

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    return true;
}

void VulkanContext::waitIdle() {
    if (m_device != VK_NULL_HANDLE && !m_deviceLost) {
        vkDeviceWaitIdle(m_device);
    }
}

bool VulkanContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 VmaMemoryUsage memUsage, VkBuffer& buffer, VmaAllocation& alloc) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = memUsage;

    VkResult result = vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &buffer, &alloc, nullptr);
    if (result != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("Failed to create buffer");
        return false;
    }
    // ERUPTION_TEST_VMA_TRACE=1: um log por buffer criado (tamanho, uso,
    // ponteiro). Cruzado com o inventario do ERUPTION_TEST_VMA_DUMP no
    // shutdown, identifica QUEM vazou pelo tamanho - foi assim que o par
    // vertex+index de 30752/36096 bytes foi rastreado.
    static const bool kTrace = std::getenv("ERUPTION_TEST_VMA_TRACE") != nullptr;
    if (kTrace) {
        ERUPTION_LOG_WARN("[VMATRACE] buffer %p size=%llu usage=0x%x",
                          static_cast<void*>(buffer),
                          static_cast<unsigned long long>(size), static_cast<unsigned>(usage));
    }
    return true;
}

bool VulkanContext::createImage(uint32_t width, uint32_t height, VkFormat format,
                                VkImageUsageFlags usage, VmaMemoryUsage memUsage,
                                VkImage& image, VmaAllocation& alloc, uint32_t mipLevels) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = memUsage;

    VkResult result = vmaCreateImage(m_allocator, &imageInfo, &allocInfo, &image, &alloc, nullptr);
    if (result != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("Failed to create image");
        return false;
    }
    return true;
}

void VulkanContext::deferFrameCleanup(std::function<void()>&& fn) {
    m_frameCleanupQueues[m_currentFrame].push_back(std::move(fn));
}

void VulkanContext::immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& fn) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_transferPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    fn(cmd);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    VkFence fence;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(m_device, &fenceInfo, nullptr, &fence);

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, fence);
    vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(m_device, fence, nullptr);
    vkFreeCommandBuffers(m_device, m_transferPool, 1, &cmd);
}

bool VulkanContext::copyImageToBuffer(VkImage image, VkBuffer buffer, uint32_t w, uint32_t h, VkImageAspectFlags aspect) {
    immediateSubmit([&](VkCommandBuffer cmd) {
        cmdImageBarrier(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, aspect);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = aspect;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {w, h, 1};

        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

        cmdImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, aspect);
    });
    return true;
}

void VulkanContext::copyImageToImage(VkCommandBuffer cmd, VkImage src, VkImage dst, uint32_t w, uint32_t h, VkImageAspectFlags aspect) {
    VkImageBlit blit{};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = { (int32_t)w, (int32_t)h, 1 };
    blit.srcSubresource.aspectMask = aspect;
    blit.srcSubresource.layerCount = 1;
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = { (int32_t)w, (int32_t)h, 1 };
    blit.dstSubresource.aspectMask = aspect;
    blit.dstSubresource.layerCount = 1;

    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
}

void VulkanContext::copyImageToImageScaled(VkCommandBuffer cmd, VkImage src, VkImage dst,
                                           uint32_t srcW, uint32_t srcH,
                                           uint32_t dstW, uint32_t dstH,
                                           VkImageAspectFlags aspect) {
    VkImageBlit blit{};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = { (int32_t)srcW, (int32_t)srcH, 1 };
    blit.srcSubresource.aspectMask = aspect;
    blit.srcSubresource.layerCount = 1;
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = { (int32_t)dstW, (int32_t)dstH, 1 };
    blit.dstSubresource.aspectMask = aspect;
    blit.dstSubresource.layerCount = 1;
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
}

void VulkanContext::cmdBeginRendering(VkCommandBuffer cmd,
                                      const std::vector<VkRenderingAttachmentInfo>& colorAttachments,
                                      const VkRenderingAttachmentInfo* depthAttachment,
                                      const VkRenderingAttachmentInfo* stencilAttachment,
                                      VkExtent2D extent,
                                      VkRenderingFlags flags) {
    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.flags = flags;
    renderingInfo.renderArea = {{0, 0}, extent};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
    renderingInfo.pColorAttachments = colorAttachments.data();
    renderingInfo.pDepthAttachment = depthAttachment;
    renderingInfo.pStencilAttachment = stencilAttachment;

    vkc::cmdBeginRendering(cmd, &renderingInfo);
}

void VulkanContext::cmdEndRendering(VkCommandBuffer cmd) {
    vkc::cmdEndRendering(cmd);
}

void VulkanContext::cmdImageBarrier(VkCommandBuffer cmd, VkImage image,
                                    VkImageLayout oldLayout, VkImageLayout newLayout,
                                    VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage,
                                    VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess,
                                    VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo depInfo{};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;

    vkc::cmdPipelineBarrier2(cmd, &depInfo);
}

void VulkanContext::cmdImageBarriers(VkCommandBuffer cmd,
                                     const std::vector<VkImageMemoryBarrier2>& barriers) {
    if (barriers.empty()) return;
    VkDependencyInfo depInfo{};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depInfo.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    depInfo.pImageMemoryBarriers = barriers.data();
    vkc::cmdPipelineBarrier2(cmd, &depInfo);
}

// Validação: sempre no build Debug (salvo ERUPTION_NO_VALIDATION) e, em
// qualquer build, com ERUPTION_VK_VALIDATION=1 (camadas + callback do motor,
// que aceita ERUPTION_VK_BREAK para parar no gdb).
static bool validationWanted() {
#ifdef ERUPTION_DEBUG
    if (std::getenv("ERUPTION_NO_VALIDATION") == nullptr) return true;
#endif
    return std::getenv("ERUPTION_VK_VALIDATION") != nullptr;
}

bool VulkanContext::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Eruption Engine";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "ERUPTION";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    // Vulkan 1.1: o motor só usa o que existe no 1.1 (o resto vai pela camada
    // vkc e por extensões), então roda também em Android antigo.
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    auto extensions = getRequiredExtensions();
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    const std::vector<const char*> validationLayers = {"VK_LAYER_KHRONOS_validation"};
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (validationWanted()) {
        if (checkValidationLayerSupport()) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();
            m_validationActive = true;   // o F3 avisa; ver Engine.cpp
        } else {
            ERUPTION_LOG_WARN("Camadas de validacao pedidas mas NAO disponiveis - esta "
                              "execucao nao tem checagem de barreira/layout. "
                              "Instale com: sudo apt install vulkan-validationlayers");
        }
        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = debugCallback;
        createInfo.pNext = &debugCreateInfo;
    } else if (std::getenv("ERUPTION_NO_VALIDATION")) {
        ERUPTION_LOG_WARN("ERUPTION_NO_VALIDATION: camadas de validacao DESLIGADAS "
                          "(nao ha' checagem de barreira/layout nesta execucao)");
    }

    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &m_instance));
    return true;
}

bool VulkanContext::setupDebugMessenger() {
    if (validationWanted()) {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;

    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
    if (func) {
        VK_CHECK(func(m_instance, &createInfo, nullptr, &m_debugMessenger));
    }
    }
    return true;
}

bool VulkanContext::createSurface(GLFWwindow* window) {
    VK_CHECK(glfwCreateWindowSurface(m_instance, window, nullptr, &m_surface));
    return true;
}

static int deviceScore(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);

    int score = 0;
    switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   score = 1000; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 500;  break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    score = 250;  break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            score = 100;  break;
        default:                                     score = 0;    break;
    }
    return score;
}

bool VulkanContext::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        ERUPTION_LOG_FATAL("No Vulkan-capable devices found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    // Log every GPU so the user can see what the system reports.
    ERUPTION_LOG_INFO("Found %u Vulkan device(s):", deviceCount);
    for (uint32_t i = 0; i < deviceCount; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        const char* typeStr = "Unknown";
        switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   typeStr = "Discrete"; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: typeStr = "Integrated"; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    typeStr = "Virtual"; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:            typeStr = "CPU"; break;
            default: break;
        }
        ERUPTION_LOG_WARN("  [%u] %s (vendor 0x%04X, device 0x%04X, type: %s)",
                        i, props.deviceName,
                        props.vendorID, props.deviceID, typeStr);
    }

    // Optional user override: ERUPTION_GPU_INDEX=N picks the N-th device.
    const char* envIndex = std::getenv("ERUPTION_GPU_INDEX");
    if (envIndex) {
        int idx = std::atoi(envIndex);
        if (idx >= 0 && static_cast<uint32_t>(idx) < deviceCount && isDeviceSuitable(devices[idx])) {
            m_physicalDevice = devices[idx];
            ERUPTION_LOG_INFO("ERUPTION_GPU_INDEX=%d override active", idx);
        } else {
            ERUPTION_LOG_WARN("ERUPTION_GPU_INDEX=%d invalid or device not suitable, using default selection", idx);
        }
    }

    if (m_physicalDevice == VK_NULL_HANDLE) {
        // Score-based selection: prefer discrete > integrated > virtual > CPU.
        std::vector<std::pair<int, VkPhysicalDevice>> candidates;
        for (const auto& device : devices) {
            if (isDeviceSuitable(device)) {
                candidates.emplace_back(deviceScore(device), device);
            }
        }

        if (!candidates.empty()) {
            std::sort(candidates.begin(), candidates.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
            m_physicalDevice = candidates.front().second;
        }
    }

    if (m_physicalDevice == VK_NULL_HANDLE) {
        ERUPTION_LOG_FATAL("No suitable Vulkan device found");
        return false;
    }

    vkGetPhysicalDeviceProperties(m_physicalDevice, &m_deviceProperties);
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &m_supportedFeatures);

    m_hasImageRobustness = deviceHasExtension(m_physicalDevice, VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME);
    m_descriptorIndexing = {};
    m_descriptorIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    m_imageRobustness = {};
    m_imageRobustness.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES;
    if (m_hasImageRobustness) m_descriptorIndexing.pNext = &m_imageRobustness;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &m_descriptorIndexing;
    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features2);
    {
        const auto& di = m_descriptorIndexing;
        const bool ext = deviceHasExtension(m_physicalDevice, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) &&
                         deviceHasExtension(m_physicalDevice, VK_KHR_MAINTENANCE3_EXTENSION_NAME);
        const char* force = std::getenv("ERUPTION_NO_BINDLESS");
        m_bindless = ext && di.runtimeDescriptorArray && di.descriptorBindingPartiallyBound &&
                     di.shaderSampledImageArrayNonUniformIndexing &&
                     di.descriptorBindingSampledImageUpdateAfterBind &&
                     di.descriptorBindingVariableDescriptorCount && !(force && force[0] == '1');
        if (!m_bindless) {
            m_descriptorIndexing = {};
            m_descriptorIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
            ERUPTION_LOG_WARN("No descriptor indexing%s: fixed texture array (%u slots, no-bindless shaders)",
                              force ? " (forced by ERUPTION_NO_BINDLESS)" : "", ERUPTION_FALLBACK_TEX_SLOTS);
        }
    }

    // Qual GPU esta em uso era invisivel: o log "Selected GPU" e' INFO e o
    // projeto compila com nivel WARNING. Cair em software (llvmpipe) ou na
    // integrada de um notebook hibrido derruba o FPS em ordens de grandeza e
    // parecia "a engine esta lenta". Agora a linha sai sempre, e os dois casos
    // ruins gritam.
    if (m_deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        ERUPTION_LOG_WARN("=========================================================");
        ERUPTION_LOG_WARN("ATENCAO: renderizando por SOFTWARE (%s).", m_deviceProperties.deviceName);
        ERUPTION_LOG_WARN("Qualquer numero de performance colhido assim NAO vale.");
        ERUPTION_LOG_WARN("=========================================================");
    } else {
        bool hasDiscrete = false;
        for (const auto& d : devices) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(d, &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { hasDiscrete = true; break; }
        }
        if (hasDiscrete && m_deviceProperties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            ERUPTION_LOG_WARN("ATENCAO: usando '%s' embora exista GPU discreta no sistema. "
                              "Em notebook hibrido isso costuma ser PRIME render offload nao ativado.",
                              m_deviceProperties.deviceName);
        }
    }
    ERUPTION_LOG_WARN("GPU em uso: %s", m_deviceProperties.deviceName);
    ERUPTION_LOG_INFO("Selected GPU: %s", m_deviceProperties.deviceName);
    ERUPTION_LOG_INFO("Max push constants size: %u", m_deviceProperties.limits.maxPushConstantsSize);
    ERUPTION_LOG_INFO("Timestamp period: %.3f ns", m_deviceProperties.limits.timestampPeriod);
    return true;
}

bool VulkanContext::createLogicalDevice() {
    m_queueFamilies = findQueueFamilies(m_physicalDevice);

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {
        m_queueFamilies.graphicsFamily,
        m_queueFamilies.presentFamily,
        m_queueFamilies.computeFamily,
        m_queueFamilies.transferFamily
    };

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        if (queueFamily == UINT32_MAX) continue;
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    // Recursos fora do 1.1, pedidos como extensão (no 1.2/1.3 as extensões
    // continuam expostas). dynamic rendering e synchronization2 não são
    // usados: a camada vkc traduz para render pass e barreira do 1.1.
    VkPhysicalDeviceDescriptorIndexingFeatures descIndexing{};
    descIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    descIndexing.runtimeDescriptorArray = m_descriptorIndexing.runtimeDescriptorArray;
    descIndexing.shaderSampledImageArrayNonUniformIndexing = m_descriptorIndexing.shaderSampledImageArrayNonUniformIndexing;
    descIndexing.descriptorBindingPartiallyBound = m_descriptorIndexing.descriptorBindingPartiallyBound;
    descIndexing.descriptorBindingSampledImageUpdateAfterBind = m_descriptorIndexing.descriptorBindingSampledImageUpdateAfterBind;
    descIndexing.descriptorBindingUpdateUnusedWhilePending = m_descriptorIndexing.descriptorBindingUpdateUnusedWhilePending;
    descIndexing.descriptorBindingVariableDescriptorCount = m_descriptorIndexing.descriptorBindingVariableDescriptorCount;
    // FSR 3.1: os passes do SDK leem alem da borda em alguns pontos (SPD mip 5,
    // RCAS, borda 3x3 do prepare_inputs). No D3D isso devolve 0; no Vulkan e'
    // indefinido sem robustImageAccess. Opcional: sem ele o FSR roda igual ao
    // backend Vulkan do proprio SDK, que tambem nao liga.
    VkPhysicalDeviceImageRobustnessFeatures robustness{};
    robustness.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES;
    robustness.robustImageAccess = m_imageRobustness.robustImageAccess;

    VkPhysicalDeviceFeatures features{};
    // Only enable these if the driver actually supports them. The engine does
    // not rely on them for core rendering, so keeping them optional improves
    // compatibility with open-source drivers and older hardware.
    features.samplerAnisotropy = m_supportedFeatures.samplerAnisotropy ? VK_TRUE : VK_FALSE;
    // G-buffer e passes com vários alvos usam blend diferente por anexo.
    features.independentBlend = m_supportedFeatures.independentBlend ? VK_TRUE : VK_FALSE;
    features.fillModeNonSolid = m_supportedFeatures.fillModeNonSolid ? VK_TRUE : VK_FALSE;
    features.wideLines = m_supportedFeatures.wideLines ? VK_TRUE : VK_FALSE;
    // FSR 3.1: a saida do upscaler e' uma storage image SEM formato no GLSL.
    features.shaderStorageImageWriteWithoutFormat =
        m_supportedFeatures.shaderStorageImageWriteWithoutFormat ? VK_TRUE : VK_FALSE;
    {
        // O SPD (piramides de luma do FSR) usa operacoes quad de subgroup em
        // compute. Sem elas (ou sem a escrita sem formato) o FSR fica
        // indisponivel e o motor volta pro FXAA.
        VkPhysicalDeviceSubgroupProperties sg{};
        sg.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &sg;
        vkGetPhysicalDeviceProperties2(m_physicalDevice, &p2);
        const bool quad = (sg.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT) &&
                          (sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT);
        m_fsrSupported = quad && m_supportedFeatures.shaderStorageImageWriteWithoutFormat &&
                         m_deviceProperties.limits.maxComputeWorkGroupInvocations >= 256;
    }
    // PIPELINE STATISTICS: e' o que responde "quantos triangulos REALMENTE
    // passaram pelo rasterizador", em vez de "quantos o arquivo tinha". Sem
    // isso, publicar numero de cena de referencia e' enganoso: o LOD de malha
    // roda no load (mesh_lod_ratio 0,45) e a contagem da FONTE nao e' a que se
    // desenha. Opcional como as outras: se o driver nao tiver, a engine roda
    // igual e a metrica so' nao aparece.
    features.pipelineStatisticsQuery =
        m_supportedFeatures.pipelineStatisticsQuery ? VK_TRUE : VK_FALSE;
    m_pipeStatsSupported = m_supportedFeatures.pipelineStatisticsQuery == VK_TRUE;
    // TESSELACAO: opcional como as de cima. Serve a banda perto do chao
    // (model.tesc/model.tese); sem ela o ModelRenderer nem cria o pipeline e
    // a cena roda exatamente como antes. E' o estagio mais mal suportado em
    // GPU fraca e driver antigo, entao NUNCA e' pre-requisito de nada.
    features.tessellationShader =
        m_supportedFeatures.tessellationShader ? VK_TRUE : VK_FALSE;
    m_tessellationSupported = m_supportedFeatures.tessellationShader == VK_TRUE;
    m_maxTessLevel = m_tessellationSupported
                         ? m_deviceProperties.limits.maxTessellationGenerationLevel
                         : 1u;

    std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (m_bindless) {
        deviceExtensions.push_back(VK_KHR_MAINTENANCE3_EXTENSION_NAME);
        deviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    }
    void* featureChain = m_bindless ? static_cast<void*>(&descIndexing) : nullptr;
    if (m_hasImageRobustness && robustness.robustImageAccess) {
        deviceExtensions.push_back(VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME);
        robustness.pNext = featureChain;
        featureChain = &robustness;
    }

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &features;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();
    createInfo.pNext = featureChain;

#ifdef ERUPTION_DEBUG
    // In modern Vulkan, device layers are deprecated and ignored.
    // They are handled at the instance level.
#endif

    VK_CHECK(vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device));
    vkc::init(m_device, MAX_FRAMES_IN_FLIGHT);

    vkGetDeviceQueue(m_device, m_queueFamilies.graphicsFamily, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_queueFamilies.presentFamily, 0, &m_presentQueue);
    if (m_queueFamilies.computeFamily != UINT32_MAX) {
        vkGetDeviceQueue(m_device, m_queueFamilies.computeFamily, 0, &m_computeQueue);
    }
    if (m_queueFamilies.transferFamily != UINT32_MAX) {
        vkGetDeviceQueue(m_device, m_queueFamilies.transferFamily, 0, &m_transferQueue);
    }

    return true;
}

bool VulkanContext::createSwapchain() {
    SwapchainSupportDetails swapchainSupport = querySwapchainSupport(m_physicalDevice);

    VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapchainSupport.formats);
    VkPresentModeKHR presentMode = chooseSwapPresentMode(swapchainSupport.presentModes);
    VkExtent2D extent = chooseSwapExtent(swapchainSupport.capabilities);

    uint32_t imageCount = swapchainSupport.capabilities.minImageCount + 1;
    if (swapchainSupport.capabilities.maxImageCount > 0 && imageCount > swapchainSupport.capabilities.maxImageCount) {
        imageCount = swapchainSupport.capabilities.maxImageCount;
    }
    // Ensure triple buffering
    if (imageCount < 3 && swapchainSupport.capabilities.maxImageCount >= 3) {
        imageCount = 3;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = m_surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    uint32_t queueFamilyIndices[] = {m_queueFamilies.graphicsFamily, m_queueFamilies.presentFamily};
    if (m_queueFamilies.graphicsFamily != m_queueFamilies.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = swapchainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    VK_CHECK(vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain));

    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
    m_swapImages.resize(imageCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapImages.data());

    // Um semaforo renderFinished POR IMAGEM do swapchain, indexado por
    // m_imageIndex no submit/present - nao por frame em voo. Indexar por
    // frame e' o erro classico (VUID-vkQueueSubmit-pSignalSemaphores-00067 nas
    // layers >= 1.3.275): com MAILBOX o motor de apresentacao segura uma
    // imagem e o seu semaforo alem do ciclo de 3 frames, entao o mesmo
    // semaforo era re-sinalizado ANTES de o present anterior consumi-lo. O
    // sintoma no monitor e' exatamente o que o autor descreveu 2026-09-02:
    // "um frame esquisito, provavelmente antigo, quando movo a camera" - o
    // present de uma imagem cujo conteudo ainda e' o do frame anterior.
    // Recriacao do swapchain roda apos waitIdle, entao so' ACRESCENTA os que
    // faltam (nunca destroi em uso).
    {
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        while (m_renderFinished.size() < m_swapImages.size()) {
            VkSemaphore sem = VK_NULL_HANDLE;
            VK_CHECK(vkCreateSemaphore(m_device, &si, nullptr, &sem));
            m_renderFinished.push_back(sem);
        }
    }

    m_swapFormat = surfaceFormat.format;
    m_swapExtent = extent;
    ERUPTION_LOG_WARN("Swapchain: %ux%u (%.2f Mpx)", extent.width, extent.height,
                      extent.width * extent.height / 1.0e6);

    return true;
}

bool VulkanContext::createImageViews() {
    m_swapViews.resize(m_swapImages.size());
    for (size_t i = 0; i < m_swapImages.size(); ++i) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = m_swapImages[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = m_swapFormat;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        VK_CHECK(vkc::createImageView(m_device, &createInfo, nullptr, &m_swapViews[i]));
    }
    return true;
}

bool VulkanContext::createAllocator() {
    VmaVulkanFunctions vulkanFunctions = {};
    vulkanFunctions.vkGetInstanceProcAddr = &vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr = &vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = m_physicalDevice;
    allocatorInfo.device = m_device;
    allocatorInfo.instance = m_instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_1;
    allocatorInfo.pVulkanFunctions = &vulkanFunctions;

    VK_CHECK(vmaCreateAllocator(&allocatorInfo, &m_allocator));
    return true;
}

bool VulkanContext::createSyncObjects() {
    m_frameFences.resize(MAX_FRAMES_IN_FLIGHT);
    m_imageAvailable.resize(MAX_FRAMES_IN_FLIGHT);
    // renderFinished e' por IMAGEM do swapchain (ver createSwapchain); aqui
    // so' garante o minimo caso o swapchain ainda nao exista.
    if (m_renderFinished.size() < MAX_FRAMES_IN_FLIGHT) m_renderFinished.resize(MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VK_CHECK(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailable[i]));
        if (m_renderFinished[i] == VK_NULL_HANDLE)
            VK_CHECK(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinished[i]));
        VK_CHECK(vkCreateFence(m_device, &fenceInfo, nullptr, &m_frameFences[i]));
    }

    return true;
}

bool VulkanContext::createCommandPools() {
    m_commandPools.resize(MAX_FRAMES_IN_FLIGHT);
    m_commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_queueFamilies.graphicsFamily;
        VK_CHECK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPools[i]));

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPools[i];
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(m_device, &allocInfo, &m_commandBuffers[i]));
    }

    // Transfer pool for immediate submit
    VkCommandPoolCreateInfo transferPoolInfo{};
    transferPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    transferPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    transferPoolInfo.queueFamilyIndex = m_queueFamilies.graphicsFamily;
    VK_CHECK(vkCreateCommandPool(m_device, &transferPoolInfo, nullptr, &m_transferPool));

    return true;
}

void VulkanContext::cleanupSwapchain() {
    for (auto& view : m_swapViews) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, view, nullptr);
        }
    }
    m_swapViews.clear();

    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

bool VulkanContext::recreateSwapchain() {
    waitIdle();

    cleanupSwapchain();

    if (!createSwapchain()) return false;
    if (!createImageViews()) return false;

    return true;
}

bool VulkanContext::checkValidationLayerSupport() {
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    const char* layerName = "VK_LAYER_KHRONOS_validation";
    for (const auto& layer : availableLayers) {
        if (std::strcmp(layer.layerName, layerName) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<const char*> VulkanContext::getRequiredExtensions() {
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

    if (validationWanted()) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    return extensions;
}

bool VulkanContext::deviceHasExtension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, exts.data());
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

bool VulkanContext::isDeviceSuitable(VkPhysicalDevice device) {
    QueueFamilyIndices indices = findQueueFamilies(device);
    if (!indices.isComplete()) return false;

    // Check device extension support
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

    std::set<std::string> requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    for (const auto& ext : availableExtensions) {
        requiredExtensions.erase(ext.extensionName);
    }
    if (!requiredExtensions.empty()) return false;

    // Check swapchain support
    SwapchainSupportDetails swapchainSupport = querySwapchainSupport(device);
    if (swapchainSupport.formats.empty() || swapchainSupport.presentModes.empty()) {
        return false;
    }

    // Check features
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(device, &features);
    if (!features.samplerAnisotropy) return false;

    // Texturas sem limite fixo por material (bindless): extensão de indexação
    // de descritores (núcleo no 1.2). Sem ela o motor ainda não tem caminho.
    VkPhysicalDeviceProperties devProps;
    vkGetPhysicalDeviceProperties(device, &devProps);
    if (devProps.apiVersion < VK_API_VERSION_1_1) return false;
    // Descriptor indexing NAO e' mais requisito: sem ele o motor usa o
    // caminho de array fixo (ver bindless()).

    // The GBuffer uses 6 color attachments; make sure the device supports them.
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    if (props.limits.maxColorAttachments < 6) return false;

    return true;
}

QueueFamilyIndices VulkanContext::findQueueFamilies(VkPhysicalDevice device) {
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphicsFamily = i;
        }
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            if (indices.computeFamily == UINT32_MAX || !(queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                indices.computeFamily = i;
            }
        }
        if (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
            if (indices.transferFamily == UINT32_MAX || !(queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                indices.transferFamily = i;
            }
        }

        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
        if (presentSupport) {
            indices.presentFamily = i;
        }
    }

    return indices;
}

SwapchainSupportDetails VulkanContext::querySwapchainSupport(VkPhysicalDevice device) {
    SwapchainSupportDetails details;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, m_surface, &details.capabilities);

    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, details.formats.data());
    }

    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &presentModeCount, details.presentModes.data());
    }

    return details;
}

VkSurfaceFormatKHR VulkanContext::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats[0];
}

VkPresentModeKHR VulkanContext::chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) {
    std::string modeList;
    for (size_t i = 0; i < modes.size(); ++i) {
        modeList += std::to_string(modes[i]);
        if (i + 1 < modes.size()) modeList += ",";
    }
    ERUPTION_LOG_DEBUG("Available present modes: %s", modeList.c_str());

    // Prefer low-latency, uncapped modes. FIFO caps the frame rate to the
    // monitor refresh rate, so we only fall back to it when nothing else is
    // available.
    auto findMode = [&](VkPresentModeKHR target, const char* name) -> VkPresentModeKHR {
        for (const auto& mode : modes) {
            if (mode == target) {
                ERUPTION_LOG_DEBUG("Selected present mode: %s", name);
                return mode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    };
    if (m_preferFifo) {
        // Explicit vsync request (battery): FIFO is always available.
        ERUPTION_LOG_DEBUG("Selected present mode: FIFO (requested)");
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    if (m_preferImmediate) {
        VkPresentModeKHR mode = findMode(VK_PRESENT_MODE_IMMEDIATE_KHR, "IMMEDIATE");
        if (mode != VK_PRESENT_MODE_FIFO_KHR) return mode;
        mode = findMode(VK_PRESENT_MODE_MAILBOX_KHR, "MAILBOX");
        if (mode != VK_PRESENT_MODE_FIFO_KHR) return mode;
    } else {
        VkPresentModeKHR mode = findMode(VK_PRESENT_MODE_MAILBOX_KHR, "MAILBOX");
        if (mode != VK_PRESENT_MODE_FIFO_KHR) return mode;
        mode = findMode(VK_PRESENT_MODE_IMMEDIATE_KHR, "IMMEDIATE");
        if (mode != VK_PRESENT_MODE_FIFO_KHR) return mode;
    }
    ERUPTION_LOG_DEBUG("Selected present mode: FIFO");
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanContext::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& cap) {
    if (cap.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return cap.currentExtent;
    }

    int width, height;
    glfwGetFramebufferSize(m_window, &width, &height);

    VkExtent2D actualExtent = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height)
    };

    actualExtent.width = std::clamp(actualExtent.width, cap.minImageExtent.width, cap.maxImageExtent.width);
    actualExtent.height = std::clamp(actualExtent.height, cap.minImageExtent.height, cap.maxImageExtent.height);
    return actualExtent;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) {
    (void)messageType;
    (void)pUserData;

    // Filter out noisy/inconsequential loader messages that are common when
    // multiple Vulkan ICDs are installed (e.g. NVIDIA + Intel + llvmpipe).
    if (pCallbackData && pCallbackData->pMessage) {
        const char* msg = pCallbackData->pMessage;
        if (std::strstr(msg, "Failed to CreateInstance in ICD") ||
            std::strstr(msg, "Skipping ICD")) {
            return VK_FALSE;
        }
    }

    // ERUPTION_VK_BREAK=<substring>: para na primeira mensagem de validacao
    // (erro ou aviso) que contenha a substring (SIGTRAP) - rodar sob gdb para
    // obter o call site.
    static const char* brk = std::getenv("ERUPTION_VK_BREAK");
    if (brk && pCallbackData->pMessage && std::strstr(pCallbackData->pMessage, brk)) {
        ERUPTION_LOG_ERROR("[Validation][BREAK] %s", pCallbackData->pMessage);
        raise(SIGTRAP);
    }
    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ERUPTION_LOG_ERROR("[Validation] %s", pCallbackData->pMessage);
    } else if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ERUPTION_LOG_WARN("[Validation] %s", pCallbackData->pMessage);
    } else {
        ERUPTION_LOG_DEBUG("[Validation] %s", pCallbackData->pMessage);
    }
    return VK_FALSE;
}

bool VulkanContext::createTimestampPools() {
    m_timestampPools.resize(MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
    m_timestampResults.resize(MAX_FRAMES_IN_FLIGHT);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkQueryPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        poolInfo.queryCount = TIMESTAMP_QUERY_COUNT;
        if (vkCreateQueryPool(m_device, &poolInfo, nullptr, &m_timestampPools[i]) != VK_SUCCESS) {
            ERUPTION_LOG_ERROR("Failed to create timestamp query pool %u", i);
            return false;
        }
    }
    return true;
}

bool VulkanContext::createPipelineStatsPools() {
    if (!m_pipeStatsSupported) return true;   // sem suporte: segue sem a metrica
    m_pipeStatsPools.resize(MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
    m_pipeStatsResults.assign(MAX_FRAMES_IN_FLIGHT,
                              std::vector<uint64_t>(PIPE_STATS_COUNT, 0));
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkQueryPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        poolInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        poolInfo.queryCount = 1;   // uma consulta: o passe de geometria
        // A ORDEM aqui define a ordem dos resultados no buffer - e' a mesma de
        // PipeStat no header. Nao reordene sem mexer la'.
        poolInfo.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (vkCreateQueryPool(m_device, &poolInfo, nullptr, &m_pipeStatsPools[i]) != VK_SUCCESS) {
            ERUPTION_LOG_WARN("Falha ao criar pool de pipeline statistics %u - metrica desligada", i);
            m_pipeStatsSupported = false;
            return true;
        }
    }
    return true;
}

void VulkanContext::destroyPipelineStatsPools() {
    for (auto& pool : m_pipeStatsPools) {
        if (pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
    }
    m_pipeStatsPools.clear();
    m_pipeStatsResults.clear();
}

void VulkanContext::resetPipelineStatsPool(VkCommandBuffer cmd) {
    if (!m_pipeStatsSupported || m_pipeStatsPools.empty()) return;
    if (m_pipeStatsPools[m_currentFrame] != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, m_pipeStatsPools[m_currentFrame], 0, 1);
    }
}

void VulkanContext::beginPipelineStats(VkCommandBuffer cmd) {
    if (!m_pipeStatsSupported || m_pipeStatsPools.empty()) return;
    if (m_pipeStatsPools[m_currentFrame] != VK_NULL_HANDLE) {
        vkCmdBeginQuery(cmd, m_pipeStatsPools[m_currentFrame], 0, 0);
        m_pipeStatsActive = true;
    }
}

void VulkanContext::endPipelineStats(VkCommandBuffer cmd) {
    if (!m_pipeStatsActive) return;
    vkCmdEndQuery(cmd, m_pipeStatsPools[m_currentFrame], 0);
    m_pipeStatsActive = false;
}

uint64_t VulkanContext::pipelineStat(PipeStat which) const {
    if (!m_pipeStatsSupported || m_pipeStatsResults.empty()) return 0;
    const auto& r = m_pipeStatsResults[m_currentFrame];
    const size_t i = static_cast<size_t>(which);
    return (i < r.size()) ? r[i] : 0;
}

void VulkanContext::destroyTimestampPools() {
    for (auto& pool : m_timestampPools) {
        if (pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
    }
    m_timestampPools.clear();
    m_timestampResults.clear();
}

void VulkanContext::resetTimestampPool(VkCommandBuffer cmd) {
    if (!m_timestampPools.empty() && m_timestampPools[m_currentFrame] != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, m_timestampPools[m_currentFrame], 0, TIMESTAMP_QUERY_COUNT);
    }
}

void VulkanContext::writeTimestamp(VkCommandBuffer cmd, uint32_t queryIndex, VkPipelineStageFlagBits stage) {
    if (queryIndex >= TIMESTAMP_QUERY_COUNT) return;
    if (!m_timestampPools.empty() && m_timestampPools[m_currentFrame] != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(cmd, stage, m_timestampPools[m_currentFrame], queryIndex);
    }
}

float VulkanContext::timestampDeltaMs(uint32_t startIndex, uint32_t endIndex) const {
    if (startIndex >= TIMESTAMP_QUERY_COUNT || endIndex >= TIMESTAMP_QUERY_COUNT) return 0.0f;
    if (m_timestampResults.empty() || m_timestampResults[m_currentFrame].size() < TIMESTAMP_QUERY_COUNT * 2) return 0.0f;
    // WITH_AVAILABILITY layout: (value, availability) pairs, stride 2 uint64.
    if (m_timestampResults[m_currentFrame][startIndex * 2 + 1] == 0 ||
        m_timestampResults[m_currentFrame][endIndex * 2 + 1] == 0) return 0.0f;
    uint64_t a = m_timestampResults[m_currentFrame][startIndex * 2];
    uint64_t b = m_timestampResults[m_currentFrame][endIndex * 2];
    if (a == 0 || b == 0 || b <= a) return 0.0f;
    double ns = static_cast<double>(b - a) * m_deviceProperties.limits.timestampPeriod;
    return static_cast<float>(ns / 1e6); // ns -> ms
}

void VulkanContext::dumpVmaStats(const char* path) const {
    if (m_allocator == VK_NULL_HANDLE || !path) return;
    char* json = nullptr;
    vmaBuildStatsString(m_allocator, &json, VK_TRUE);
    if (!json) return;
    if (FILE* f = std::fopen(path, "wb")) { std::fputs(json, f); std::fclose(f); }
    vmaFreeStatsString(m_allocator, json);
}

} // namespace eruption
