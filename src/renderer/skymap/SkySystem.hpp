#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/skymap/SkyConfig.hpp"
#include "math/Types.hpp"
#include "utils/DayNightCycle.hpp"
#include <memory>

namespace eruption {

class ISkyBackend;

class SkySystem {
public:
    SkySystem();
    ~SkySystem();

    bool init(VulkanContext* ctx, uint32_t width, uint32_t height);
    void shutdown();
    void resize(uint32_t width, uint32_t height);

    void loadConfig(const SkyConfig& config);
    void reloadConfig(const SkyConfig& config);

    // Render sky into active framebuffer (background fill).
    void render(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& viewProj);

    // Render sky off-screen to a sampled texture.
    void renderToTexture(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& viewProj);
    // Sonda de ceu (G36) - ver ISkyBackend::renderProbeFace.
    bool renderProbeFace(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& invViewProj,
                         VkImageView faceView, VkImageView depthView, uint32_t size, uint32_t face);

    Vec3 getSkyColor(const Vec3& direction, const DayNightCycle& cycle) const;

    VkImageView outputView() const;
    VkSampler outputSampler() const;

    const char* currentBackendName() const;
    SkyBackendType currentBackendType() const { return m_config.backend; }
    const SkyConfig& config() const { return m_config; }
    SkyConfig& editableConfig() { return m_config; }
    void commitConfig();
    void setBackend(SkyBackendType type);

private:
    void recreateBackend();

    VulkanContext* m_ctx = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    SkyConfig m_config;
    std::unique_ptr<ISkyBackend> m_backend;
};

} // namespace eruption
