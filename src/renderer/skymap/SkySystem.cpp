#include "renderer/skymap/SkySystem.hpp"
#include "renderer/skymap/ISkyBackend.hpp"
#include "renderer/skymap/ProceduralSkyBackend.hpp"
#include "core/Logger.hpp"

namespace eruption {

SkySystem::SkySystem() = default;
SkySystem::~SkySystem() { shutdown(); }

bool SkySystem::init(VulkanContext* ctx, uint32_t width, uint32_t height) {
    m_ctx = ctx;
    m_width = width;
    m_height = height;
    recreateBackend();
    return m_backend != nullptr;
}

void SkySystem::shutdown() {
    if (m_backend) {
        m_backend->shutdown();
        m_backend.reset();
    }
    m_ctx = nullptr;
}

void SkySystem::resize(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
    if (m_backend) {
        m_backend->resize(width, height);
    }
}

void SkySystem::loadConfig(const SkyConfig& config) {
    m_config = config;
    recreateBackend();
}

void SkySystem::reloadConfig(const SkyConfig& config) {
    if (m_config.backend != config.backend) {
        loadConfig(config);
    } else {
        m_config = config;
        if (m_backend) {
            m_backend->reloadConfig(m_config);
        }
    }
}

void SkySystem::recreateBackend() {
    if (m_backend) {
        m_backend->shutdown();
        m_backend.reset();
    }

    switch (m_config.backend) {
        case SkyBackendType::CloudLayers:
            // Cloud layers are rendered by the engine on top of the scene;
            // keep a procedural sky background.
            m_backend = std::make_unique<ProceduralSkyBackend>(m_config);
            break;
        case SkyBackendType::CloudFluff:
            // CloudFluff was removed; fall back to procedural.
            ERUPTION_LOG_WARN("CloudFluff backend removed, falling back to procedural");
            m_backend = std::make_unique<ProceduralSkyBackend>(m_config);
            break;
        case SkyBackendType::Volumetric:
            // TODO: implement VolumetricSkyBackend
            ERUPTION_LOG_WARN("Volumetric backend not implemented yet, falling back to procedural");
            m_backend = std::make_unique<ProceduralSkyBackend>(m_config);
            break;
        case SkyBackendType::Procedural:
        default:
            m_backend = std::make_unique<ProceduralSkyBackend>(m_config);
            break;
    }

    if (m_backend && m_ctx) {
        m_backend->init(m_ctx, m_width, m_height);
    }
}

void SkySystem::render(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& viewProj) {
    if (m_backend) {
        m_backend->render(cmd, cycle, viewProj);
    }
}

void SkySystem::renderToTexture(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& viewProj) {
    if (m_backend) {
        m_backend->renderToTexture(cmd, cycle, viewProj);
    }
}

bool SkySystem::renderProbeFace(VkCommandBuffer cmd, const DayNightCycle& cycle, const Mat4& invViewProj,
                                VkImageView faceView, VkImageView depthView, uint32_t size, uint32_t face) {
    if (!m_backend) return false;
    return m_backend->renderProbeFace(cmd, cycle, invViewProj, faceView, depthView, size, face);
}

Vec3 SkySystem::getSkyColor(const Vec3& direction, const DayNightCycle& cycle) const {
    if (m_backend) {
        return m_backend->getSkyColor(direction, cycle);
    }
    return Vec3(0.0f);
}

VkImageView SkySystem::outputView() const {
    if (m_backend) return m_backend->outputView();
    return VK_NULL_HANDLE;
}

VkSampler SkySystem::outputSampler() const {
    if (m_backend) return m_backend->outputSampler();
    return VK_NULL_HANDLE;
}

const char* SkySystem::currentBackendName() const {
    if (m_backend) return m_backend->name();
    return "none";
}

void SkySystem::commitConfig() {
    reloadConfig(m_config);
}

void SkySystem::setBackend(SkyBackendType type) {
    if (m_config.backend == type) return;
    m_config.backend = type;
    recreateBackend();
}

} // namespace eruption
