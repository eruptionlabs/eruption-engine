#pragma once

#include "renderer/VulkanContext.hpp"
#include "math/Types.hpp"
#include "math/Frustum.hpp"
#include <vector>

namespace eruption {

enum class LayerType {
    Sky, FarBackground, NearBackground, Midground, Foreground, Weather,
};

struct ParallaxLayer {
    LayerType type;
    float parallaxFactor = 0.0f;
    float depthOffset = 0.0f;
    float saturation = 1.0f;
    float brightness = 1.0f;
    float fogBlend = 0.0f;
    Vec3 tintColor = Vec3(1.0f);

    // Sprite data for this layer
    std::vector<Mat4> transforms;
    std::vector<Vec4> texRects;
    std::vector<uint32_t> texIndices;

    Vec2 calculateOffset(const Vec2& cameraPos) const {
        return cameraPos * parallaxFactor;
    }
};

class ParallaxSystem {
public:
    void init();
    void shutdown();

    void addLayer(const ParallaxLayer& layer);
    void clearLayers();

    void update(const Vec2& cameraPos);
    void render(VkCommandBuffer cmd);

    std::vector<const ParallaxLayer*> getVisibleLayers(const Frustum& frustum) const;

    // Atmospheric perspective helper
    static Vec3 applyAtmospheric(const Vec3& color, float distance,
                                  const Vec3& fogColor, float fogDensity);

private:
    std::vector<ParallaxLayer> m_layers;
    void sortLayers();
};

} // namespace eruption
