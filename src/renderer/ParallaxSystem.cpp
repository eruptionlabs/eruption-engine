#include "renderer/ParallaxSystem.hpp"
#include <algorithm>

namespace eruption {

void ParallaxSystem::init() {
    m_layers.reserve(8);
}

void ParallaxSystem::shutdown() {
    m_layers.clear();
}

void ParallaxSystem::addLayer(const ParallaxLayer& layer) {
    m_layers.push_back(layer);
    sortLayers();
}

void ParallaxSystem::clearLayers() {
    m_layers.clear();
}

void ParallaxSystem::sortLayers() {
    std::sort(m_layers.begin(), m_layers.end(),
              [](const ParallaxLayer& a, const ParallaxLayer& b) {
                  return a.depthOffset < b.depthOffset;
              });
}

void ParallaxSystem::update(const Vec2& cameraPos) {
    // Layer offsets are computed on-the-fly during render
    (void)cameraPos;
}

void ParallaxSystem::render(VkCommandBuffer cmd) {
    (void)cmd;
    // Placeholder: actual rendering would use a sprite/quad renderer per layer
}

std::vector<const ParallaxLayer*> ParallaxSystem::getVisibleLayers(const Frustum& frustum) const {
    (void)frustum;
    std::vector<const ParallaxLayer*> visible;
    visible.reserve(m_layers.size());
    for (const auto& layer : m_layers) {
        visible.push_back(&layer);
    }
    return visible;
}

Vec3 ParallaxSystem::applyAtmospheric(const Vec3& color, float distance,
                                       const Vec3& fogColor, float fogDensity) {
    float t = 1.0f - std::exp(-distance * fogDensity);
    float luma = color.r * 0.299f + color.g * 0.587f + color.b * 0.114f;
    Vec3 desat = glm::mix(color, Vec3(luma), t * 0.3f);
    return glm::mix(desat, fogColor, t);
}

} // namespace eruption
