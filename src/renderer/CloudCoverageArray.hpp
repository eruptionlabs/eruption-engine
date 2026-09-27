#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/CloudCoverageNoise.hpp"
#include "renderer/LocalCloud.hpp"
#include "math/Types.hpp"

namespace eruption {

// 2D texture array where each slice stores cloud coverage at a fixed altitude,
// plus a 2D altitude map that assigns a per-pixel cloud-center height so dense
// clouds sit lower and light clouds float higher.
//
// Generation is performed on the GPU via a compute shader. This avoids the
// CPU→GPU stall that happened when the coverage texture was regenerated every
// time the Cloud Amount slider moved.
class CloudCoverageArray : public CloudCoverageNoise {
public:
    static constexpr uint32_t MIN_LAYERS = 1;
    static constexpr uint32_t MAX_LAYERS = 9;
    static constexpr uint32_t DEFAULT_LAYERS = 5;
    static constexpr uint32_t MIN_SIZE = 256;
    static constexpr uint32_t MAX_SIZE = 2048;
    static constexpr uint32_t DEFAULT_SIZE = 1024;

    CloudCoverageArray() = default;
    ~CloudCoverageArray();

    bool init(VulkanContext* ctx,
              uint32_t layerCount,
              uint32_t size,
              const Vec3& worldMin,
              const Vec3& worldMax,
              uint32_t octaves,
              float coverage,
              uint32_t seed,
              float cloudBottom = 500.0f,
              float layerSpacing = 200.0f,
              VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT);

    void shutdown();

    // Records the compute generation pass into the current frame command buffer.
    // Call at the start of the frame before any pass reads the texture.
    void recordComputeGeneration(VkCommandBuffer cmd);

    // True if coverage/seed/octaves/layer params changed and need GPU regen.
    bool isDirty() const { return m_coverageDirty || m_paramsDirty; }

    // Update wind offset. Wind is applied as a UV shift in shaders; no regen.
    void update(float dt, const Vec3& windDirection, float windSpeed);
    // Wind offset only (no local-cloud drift / no texture regen). Used by
    // independent cloud layers: the cloud drifts on screen via the UV shift
    // until it leaves the map, without regenerating the coverage texture.
    void updateWind(float dt, const Vec3& windDirection, float windSpeed, float uvFactor = 0.02f);

    // Local cloud patches. They are blended on top of procedural coverage.
    void addLocalCloud(const LocalCloud& cloud);
    void clearLocalClouds();
    void updateLocalClouds(float dt, const Vec3& windDirection, float windSpeed);
    const std::vector<LocalCloud>& localClouds() const { return m_localClouds; }

    // GPU resources.
    VkImageView imageView() const { return m_imageView; }
    VkSampler sampler() const { return m_sampler; }
    VkImageView altitudeView() const { return m_altitudeView; }
    VkSampler altitudeSampler() const { return m_altitudeSampler; }
    // SSBO with the baked local-cloud blobs (read by coverage_gen.comp and by
    // the volumetric puff shader).
    VkBuffer localCloudBuffer() const { return m_localCloudBuffer; }

    // Configuration.
    void setLayerCount(uint32_t layers);
    void setSize(uint32_t size);
    void setOctaves(uint32_t octaves);
    void setCoverage(float coverage);
    void setSeed(uint32_t seed);
    void setWorldBounds(const Vec3& worldMin, const Vec3& worldMax);
    void setLayerSpacing(float spacing);
    void setCloudBottom(float bottom);

    Vec2 windOffset() const { return m_windOffset; }
    // Teleports the cloud without rebaking: the wind offset shifts where the
    // baked noise is sampled (used by the weather field's upwind respawn).
    void setWindOffset(const Vec2& v) { m_windOffset = v; }
    Vec3 worldMin() const { return CloudCoverageNoise::m_worldMin; }
    Vec3 worldMax() const { return CloudCoverageNoise::m_worldMax; }

    uint32_t layerCount() const { return m_layerCount; }
    uint32_t size() const { return m_size; }
    float layerSpacing() const { return m_layerSpacing; }
    float cloudBottom() const { return m_cloudBottom; }
    float cloudTop() const { return m_cloudBottom + (m_layerCount <= 1 ? m_layerSpacing : (m_layerCount - 1) * m_layerSpacing); }

private:
    VulkanContext* m_ctx = nullptr;

    uint32_t m_layerCount = DEFAULT_LAYERS;
    uint32_t m_size = DEFAULT_SIZE;
    float m_layerSpacing = 200.0f;
    float m_cloudBottom = 500.0f;
    // REPEAT for the global layer (infinite noise), CLAMP_TO_BORDER for
    // independent cloud layers (the cloud exists once, no tiling).
    VkSamplerAddressMode m_addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;

    bool m_coverageDirty = true;
    bool m_paramsDirty = true;
    bool m_firstGeneration = true;

    VkImage m_image = VK_NULL_HANDLE;
    VmaAllocation m_alloc = VK_NULL_HANDLE;
    VkImageView m_imageView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    // Per-pixel cloud center altitude (R16) used to vary cloud height per region.
    VkImage m_altitudeImage = VK_NULL_HANDLE;
    VmaAllocation m_altitudeAlloc = VK_NULL_HANDLE;
    VkImageView m_altitudeView = VK_NULL_HANDLE;
    VkSampler m_altitudeSampler = VK_NULL_HANDLE;

    Vec2 m_lastWindOffset = Vec2(0.0f);

    // Local clouds GPU buffer.
    std::vector<LocalCloud> m_localClouds;
    VkBuffer m_localCloudBuffer = VK_NULL_HANDLE;
    VmaAllocation m_localCloudAlloc = VK_NULL_HANDLE;
    bool m_localCloudsDirty = true;

    // Compute generation pipeline.
    VkPipeline m_computePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_computePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_computeDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_computeDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_computeDescSet = VK_NULL_HANDLE;

    bool createImage();
    void destroyImage();
    bool createSampler();
    void destroySampler();
    bool createAltitudeImage();
    void destroyAltitudeImage();
    bool createAltitudeSampler();
    void destroyAltitudeSampler();

    bool createComputePipeline();
    void destroyComputePipeline();
    void dispatchCompute(VkCommandBuffer cmd);

    bool createLocalCloudBuffer();
    void destroyLocalCloudBuffer();
    void updateLocalCloudBuffer();

    // Vertical amplitude profile.  t in [0,1], return in [0,1].
    static float layerProfile(float t);
};

} // namespace eruption
