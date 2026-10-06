#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <vector>
#include <string>
#include <functional>
#include <cstdint>

struct GLFWwindow;

namespace eruption {

#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult err = x;                                               \
        if (err != VK_SUCCESS) {                                        \
            Logger::error("Vulkan error: %d at %s:%d", err, __FILE__, __LINE__); \
            return false;                                               \
        }                                                               \
    } while (0)

#define VK_CHECK_VOID(x)                                                \
    do {                                                                \
        VkResult err = x;                                               \
        if (err != VK_SUCCESS) {                                        \
            Logger::error("Vulkan error: %d at %s:%d", err, __FILE__, __LINE__); \
            return;                                                     \
        }                                                               \
    } while (0)

struct QueueFamilyIndices {
    uint32_t graphicsFamily = UINT32_MAX;
    uint32_t presentFamily = UINT32_MAX;
    uint32_t computeFamily = UINT32_MAX;
    uint32_t transferFamily = UINT32_MAX;

    bool isComplete() const {
        return graphicsFamily != UINT32_MAX && presentFamily != UINT32_MAX;
    }
};

struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class VulkanContext {
public:
    bool init(GLFWwindow* window);
    void shutdown();

    GLFWwindow* window() const { return m_window; }

    void preferImmediatePresentMode(bool enable) { m_preferImmediate = enable; }
    // FIFO = vsync, the battery-friendly mode (GPU idles between refreshes).
    // Returns true when the preference changed (caller should recreate the
    // swapchain for it to take effect).
    bool setPresentPreference(bool preferFifo, bool preferImmediate) {
        const bool changed = (m_preferFifo != preferFifo) || (m_preferImmediate != preferImmediate);
        m_preferFifo = preferFifo;
        m_preferImmediate = preferImmediate;
        return changed;
    }

    bool beginFrame();
    bool endFrame();
    void waitIdle();
    bool isDeviceLost() const { return m_deviceLost; }
    bool isFrameRecording() const { return m_isFrameRecording; }

    // Accessors
    VkDevice device() const { return m_device; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkInstance instance() const { return m_instance; }
    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    VkQueue presentQueue() const { return m_presentQueue; }
    VkQueue computeQueue() const { return m_computeQueue; }
    VkQueue transferQueue() const { return m_transferQueue; }
    VmaAllocator allocator() const { return m_allocator; }
    VkCommandBuffer currentCmdBuf() const { return m_commandBuffers[m_currentFrame]; }

    // Disk-backed pipeline cache (.cache/eruption/pipeline_cache.bin): warm
    // launches skip driver pipeline recompilation. The static accessor serves
    // call sites that only hold a raw VkDevice (PipelineBuilder etc.).
    VkPipelineCache pipelineCache() const { return m_pipelineCache; }
    static VkPipelineCache globalPipelineCache() { return s_pipelineCache; }

    // Swapchain
    VkExtent2D swapExtent() const { return m_swapExtent; }
    VkFormat swapFormat() const { return m_swapFormat; }
    uint32_t swapImageCount() const { return static_cast<uint32_t>(m_swapImages.size()); }
    VkSwapchainKHR swapchain() const { return m_swapchain; }
    VkImageView swapImageView(uint32_t index) const { return m_swapViews[index]; }
    VkImage swapImage(uint32_t index) const { return m_swapImages[index]; }

    // Per-frame resources
    // As camadas de validacao estao ATIVAS nesta execucao? O F3 precisa saber:
    // elas custam caro e o custo cai todo em "render (record)", o que faz o
    // painel parecer contraditorio (CPU alta, passes de GPU baixos).
    bool validationActive() const { return m_validationActive; }

    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

    // Custo do vkQueuePresentKHR do ultimo frame. Sob xvfb isso e' ~7,4 ms e
    // domina o frame inteiro; num swapchain real e' proximo de zero. Exposto
    // para a telemetria poder separar "a engine esta lenta" de "o ambiente de
    // teste esta lento".
    float lastPresentMs() const { return m_lastPresentMs; }
    float m_lastPresentMs = 0.0f;

    // Acumuladores do ERUPTION_DEBUG_PRESENT (custo fixo por frame).
    double m_dbgFenceMs = 0.0, m_dbgAcquireMs = 0.0, m_dbgPresentMs = 0.0;
    uint64_t m_dbgFrames = 0;
    uint32_t currentFrame() const { return m_currentFrame; }
    uint32_t currentImageIndex() const { return m_imageIndex; }
    VkFence currentFrameFence() const { return m_frameFences[m_currentFrame]; }
    VkFence frameFence(uint32_t idx) const { return m_frameFences[idx % MAX_FRAMES_IN_FLIGHT]; }
    VkSemaphore currentImageAvailable() const { return m_imageAvailable[m_currentFrame]; }
    VkSemaphore currentRenderFinished() const { return m_renderFinished[m_currentFrame]; }
    const QueueFamilyIndices& queueFamilies() const { return m_queueFamilies; }

    // Memory helpers
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VmaMemoryUsage memUsage, VkBuffer& buffer, VmaAllocation& alloc);
    bool createImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageUsageFlags usage, VmaMemoryUsage memUsage,
                     VkImage& image, VmaAllocation& alloc, uint32_t mipLevels = 1);
    // Tamanho REAL da alocacao (com alinhamento/padding do VMA). E' o que os
    // PROFILE_VRAM_FREE ja' usam; o ALLOC tem que espelhar para bater.
    // Grava vmaBuildStatsString (JSON com cada alocacao viva) em `path`.
    void dumpVmaStats(const char* path) const;
    VkDeviceSize allocationSize(VmaAllocation alloc) const {
        if (alloc == VK_NULL_HANDLE) return 0;
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(m_allocator, alloc, &info);
        return info.size;
    }

    // Synchronization helpers
    void immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& fn);
    void deferFrameCleanup(std::function<void()>&& fn);

    // Image data recovery
    bool copyImageToBuffer(VkImage image, VkBuffer buffer, uint32_t w, uint32_t h, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
    void copyImageToImage(VkCommandBuffer cmd, VkImage src, VkImage dst, uint32_t w, uint32_t h, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
    // Versao com tamanho de ORIGEM e DESTINO distintos: e' o que permite
    // renderizar abaixo da resolucao da janela e subir a imagem no blit final
    // (render scale). Ja' usa VK_FILTER_LINEAR, entao a escala sai de graca.
    void copyImageToImageScaled(VkCommandBuffer cmd, VkImage src, VkImage dst,
                                uint32_t srcW, uint32_t srcH,
                                uint32_t dstW, uint32_t dstH,
                                VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

    // Dynamic rendering helpers
    void cmdBeginRendering(VkCommandBuffer cmd,
                           const std::vector<VkRenderingAttachmentInfo>& colorAttachments,
                           const VkRenderingAttachmentInfo* depthAttachment,
                           const VkRenderingAttachmentInfo* stencilAttachment,
                           VkExtent2D extent,
                           VkRenderingFlags flags = 0);
    void cmdEndRendering(VkCommandBuffer cmd);

    // Pipeline barriers
    void cmdImageBarrier(VkCommandBuffer cmd, VkImage image,
                         VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage,
                         VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess,
                         VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
    void cmdImageBarriers(VkCommandBuffer cmd,
                          const std::vector<VkImageMemoryBarrier2>& barriers);

    // Swapchain control (public for manual resize)
    void cleanupSwapchain();
    bool recreateSwapchain();

private:
    // Window handle
    GLFWwindow* m_window = nullptr;

    // Instance
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;

    // Surface
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;

    // Device
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    static VkPipelineCache s_pipelineCache;
    void createPipelineCache();
    void destroyPipelineCache();

    // Queues
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;
    VkQueue m_computeQueue = VK_NULL_HANDLE;
    VkQueue m_transferQueue = VK_NULL_HANDLE;
    QueueFamilyIndices m_queueFamilies;

    // Swapchain
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_swapExtent = {};
    std::vector<VkImage> m_swapImages;
    std::vector<VkImageView> m_swapViews;

    // VMA
    VmaAllocator m_allocator = VK_NULL_HANDLE;

    // Per-frame resources (triplo buffering)
    static constexpr uint32_t TIMESTAMP_QUERY_COUNT = 64;
    std::vector<VkCommandPool> m_commandPools;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VkFence> m_frameFences;
    std::vector<VkSemaphore> m_imageAvailable;
    std::vector<VkSemaphore> m_renderFinished;
    std::vector<std::function<void()>> m_frameCleanupQueues[MAX_FRAMES_IN_FLIGHT];
    std::vector<VkQueryPool> m_pipeStatsPools;
    std::vector<std::vector<uint64_t>> m_pipeStatsResults;
    bool m_pipeStatsSupported = false;
    bool m_tessellationSupported = false;
    bool m_fsrSupported = false;
    float m_textureLodBias = 0.0f;
    // Teto de subdivisao do DEVICE (limits.maxTessellationGenerationLevel).
    // 64 no desktop, mas o alvo (930M / driver velho) e' onde o tessellator e'
    // pior suportado e o limite pode ser menor - a curva do F2 ia ate' 16 sem
    // consultar ninguem. Tefra 2026-09-06, G2.
    uint32_t m_maxTessLevel = 1;
public:
    bool tessellationSupported() const { return m_tessellationSupported; }
    // FSR 3.1 utilizavel neste device (subgroup quad em compute, escrita de
    // storage image sem formato, grupo de 256 invocacoes).
    bool fsrSupported() const { return m_fsrSupported; }
    // Vies de mip das texturas de CENA (mipLodBias dos samplers de modelo,
    // terreno e do sampler padrao). Negativo com FSR: a cena e' rasterizada
    // abaixo da resolucao de saida, e o mip tem que ser o da SAIDA, senao a
    // textura chega borrada ao upscaler. Lido na criacao dos samplers.
    void setTextureLodBias(float bias) { m_textureLodBias = bias; }
    float textureLodBias() const { return m_textureLodBias; }
    uint32_t maxTessellationLevel() const { return m_maxTessLevel; }
private:
    bool m_pipeStatsActive = false;
    std::vector<VkQueryPool> m_timestampPools;
    std::vector<std::vector<uint64_t>> m_timestampResults;
    uint32_t m_currentFrame = 0;
    uint32_t m_imageIndex = 0;
    bool m_isFrameRecording = false;
    bool m_framebufferResized = false;
    bool m_deviceLost = false;
    bool m_preferImmediate = false;
    bool m_preferFifo = false;

    VkPhysicalDeviceProperties m_deviceProperties{};
    VkPhysicalDeviceFeatures m_supportedFeatures{};
    VkPhysicalDeviceVulkan12Features m_supportedFeatures12{};
    VkPhysicalDeviceVulkan13Features m_supportedFeatures13{};

    // Transfer pool for immediate submit
    VkCommandPool m_transferPool = VK_NULL_HANDLE;

    // Private methods
    bool createInstance();
    bool setupDebugMessenger();
    bool createSurface(GLFWwindow* window);
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createSwapchain();
    bool createImageViews();
    bool createAllocator();
    bool createSyncObjects();
    bool createCommandPools();
    bool createTimestampPools();
    void destroyTimestampPools();
    bool createPipelineStatsPools();
    void destroyPipelineStatsPools();

    // Validation
    bool m_validationActive = false;
    bool checkValidationLayerSupport();
    std::vector<const char*> getRequiredExtensions();

    // Device selection
public:
    void resetTimestampPool(VkCommandBuffer cmd);

    // PIPELINE STATISTICS (contagem do que o PIPELINE de fato processou).
    //
    // Existe porque a contagem de triangulo que a engine reportava era a do
    // que ela SUBMETE (contador de CPU, pos-culling e pos-LOD) ou, pior, a do
    // arquivo de origem. Nenhuma das duas responde "quanto foi rasterizado":
    // o LOD de malha roda no load (mesh_lod_ratio 0,45 nos tres presets),
    // entao publicar "Rungholt, 5,8 M de triangulos" comparando com benchmark
    // de outra engine seria enganoso - nao e' a contagem que se desenha.
    // Isto aqui vem do driver e nao tem como mentir.
    //
    // Vem de brinde a contagem de invocacao do fragment shader, que e' medida
    // direta de SOBREDESENHO - o eixo que faltava pra separar "custa por
    // geometria" de "custa por pixel" no orcamento do G-buffer.
    //
    // Opcional: se o driver nao expuser pipelineStatisticsQuery, tudo devolve
    // 0 e a engine roda igual.
    enum class PipeStat : uint32_t {
        InputPrimitives = 0,      // primitivas alimentadas na entrada
        VertexInvocations = 1,    // execucoes do vertex shader
        ClippedPrimitives = 2,    // primitivas que sobreviveram ao clipping
        FragmentInvocations = 3,  // execucoes do fragment shader (sobredesenho)
    };
    static constexpr uint32_t PIPE_STATS_COUNT = 4;
    void resetPipelineStatsPool(VkCommandBuffer cmd);
    void beginPipelineStats(VkCommandBuffer cmd);
    void endPipelineStats(VkCommandBuffer cmd);
    uint64_t pipelineStat(PipeStat which) const;
    bool pipelineStatsSupported() const { return m_pipeStatsSupported; }
    void writeTimestamp(VkCommandBuffer cmd, uint32_t queryIndex, VkPipelineStageFlagBits stage);
    float timestampDeltaMs(uint32_t startIndex, uint32_t endIndex) const;

private:
    bool isDeviceSuitable(VkPhysicalDevice device);
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device);

    // Helpers
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats);
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& cap);

    // Debug
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData);
};

} // namespace eruption
