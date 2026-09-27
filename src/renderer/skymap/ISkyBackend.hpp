#pragma once

#include "renderer/VulkanContext.hpp"
#include "math/Types.hpp"
#include "utils/DayNightCycle.hpp"

namespace eruption {

// Interface for pluggable sky rendering backends.
// All methods are called from the main render thread.
class ISkyBackend {
public:
    virtual ~ISkyBackend() = default;

    virtual const char* name() const = 0;

    virtual bool init(VulkanContext* ctx, uint32_t width, uint32_t height) = 0;
    virtual void shutdown() = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;

    // Render the sky into the currently bound framebuffer (background fill).
    virtual void render(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& viewProj) = 0;

    // Render the sky off-screen to a sampled texture (used by water reflections, etc.).
    virtual void renderToTexture(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& viewProj) = 0;
    // SONDA DE CEU (G36): desenha o ceu numa face de cubemap ja' em
    // COLOR_ATTACHMENT_OPTIMAL (o chamador cuida das barreiras). `invViewProj`
    // vai DIRETO ao shader (nao e' invertido aqui). Sem disco solar/lua/estrelas
    // - o sol e' luz direcional e nao pode entrar duas vezes. `face` escolhe
    // qual UBO usar: as 6 faces vao no mesmo command buffer, entao um UBO so'
    // nao serve. Default: backend nao suporta.
    virtual bool renderProbeFace(VkCommandBuffer, const DayNightCycle&, const Mat4& /*invViewProj*/,
                                 VkImageView /*faceView*/, VkImageView /*depthView*/,
                                 uint32_t /*size*/, uint32_t /*face*/) { return false; }

    // Approximate sky color for a world-space direction (fallback for non-sampled code).
    virtual Vec3 getSkyColor(const Vec3& direction, const DayNightCycle& cycle) const = 0;

    // Returns the off-screen texture view, or VK_NULL_HANDLE if not supported.
    virtual VkImageView outputView() const { return VK_NULL_HANDLE; }
    virtual VkSampler outputSampler() const { return VK_NULL_HANDLE; }

    // Called when the sky configuration changes (hot-reload).
    virtual void reloadConfig(const class SkyConfig& config) = 0;
};

} // namespace eruption
