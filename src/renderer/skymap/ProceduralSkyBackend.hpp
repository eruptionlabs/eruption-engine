#pragma once

#include "renderer/skymap/ISkyBackend.hpp"
#include "renderer/skymap/SkyConfig.hpp"

namespace eruption {

class ProceduralSkyBackend : public ISkyBackend {
public:
    ProceduralSkyBackend() = default;
    explicit ProceduralSkyBackend(const SkyConfig& config);

    const char* name() const override { return "procedural"; }

    bool init(VulkanContext* ctx, uint32_t width, uint32_t height) override;
    void shutdown() override;
    void resize(uint32_t width, uint32_t height) override;

    void render(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& viewProj) override;
    void renderToTexture(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& viewProj) override;
    bool renderProbeFace(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& invViewProj,
                         VkImageView faceView, VkImageView depthView,
                         uint32_t size, uint32_t face) override;

    Vec3 getSkyColor(const Vec3& direction, const DayNightCycle& cycle) const override;

    VkImageView outputView() const override { return m_outputView; }
    VkSampler outputSampler() const override { return m_outputSampler; }

    void reloadConfig(const SkyConfig& config) override;

private:
    void createOutputImage();
    void createDepthImage();
    void createPipeline();
    void createUBO();
    void destroyUBO();
    void updateUBO(const DayNightCycle& cycle, const Mat4& viewProj);

    VulkanContext* m_ctx = nullptr;

    VkImage m_outputImage = VK_NULL_HANDLE;
    VmaAllocation m_outputAlloc = VK_NULL_HANDLE;
    VkImageView m_outputView = VK_NULL_HANDLE;
    VkSampler m_outputSampler = VK_NULL_HANDLE;

    VkImage m_depthImage = VK_NULL_HANDLE;
    VmaAllocation m_depthAlloc = VK_NULL_HANDLE;
    VkImageView m_depthView = VK_NULL_HANDLE;

    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_uboLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_uboPool = VK_NULL_HANDLE;
    VkDescriptorSet m_uboSet = VK_NULL_HANDLE;
    VkBuffer m_uboBuffer = VK_NULL_HANDLE;
    VmaAllocation m_uboAlloc = VK_NULL_HANDLE;
    void* m_uboMapped = nullptr;
    // Sonda de ceu: um UBO + set por face (6 draws no mesmo command buffer).
    static constexpr uint32_t kProbeFaces = 6;
    VkBuffer m_probeUbo[kProbeFaces] = {};
    VmaAllocation m_probeUboAlloc[kProbeFaces] = {};
    void* m_probeUboMapped[kProbeFaces] = {};
    VkDescriptorSet m_probeSets[kProbeFaces] = {};

    uint32_t m_width = 0;
    uint32_t m_height = 0;

    SkyConfig m_config;
};

} // namespace eruption
