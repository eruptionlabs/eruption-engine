#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/WeatherTypes.hpp"
#include "utils/FastArena.hpp"
#include "renderer/CloudCoverageNoise.hpp"
#include "math/Types.hpp"
#include "math/Frustum.hpp"

namespace eruption {

// Estrutura armazenada em GPU (uma por partícula)
struct WeatherParticleGPU {
    Vec4 origin;   // xyz = posição de spawn no mundo, w = seed
    Vec4 velocity; // xyz = velocidade (m/s), w = tamanho base
    Vec4 data;     // x = tipo (0=rain,1=snow,2=hail,3=freezingRain,4=sand,5=dust), y = turbulence seed, z = lifetime scale, w = fadeIn
};

struct WeatherSplashGPU {
    Vec4 origin; // xyz = world position, w = seed
    Vec4 data;   // x = lifetime scale, y = phase, z = size, w = intensity
};

class TerrainRenderer;
class ModelRenderer;
class SpriteRenderer;
class BindlessDescriptor;
struct FrameUBO;

class WeatherRenderer {
public:
    // Safety caps; actual counts are computed dynamically from the cloud shadow
    // map area and weather intensity, and the GPU buffer is resized on demand.
    // Caps reduced from 1.5M/32k to keep thunderstorm lightweight on a 60 FPS
    // target. Still enough for a dense downpour without the CPU/GPU stalls.
    static constexpr uint32_t MAX_RAIN_PARTICLES = 393216;
    static constexpr uint32_t MAX_SNOW_PARTICLES = 4096;
    static constexpr uint32_t MAX_DUST_PARTICLES = 32768;
    static constexpr uint32_t MAX_SPLASH_PARTICLES = 8192;

    struct RenderParams {
        Mat4 view;
        Mat4 proj;
        Mat4 invViewProj;
        Vec3 cameraPos;
        // World XZ (and Y) the rain heightmap + camera-relative splash group
        // anchor to. Sentinel y < -1e8 = unset (falls back to cameraPos). The
        // gameplay camera orbits hundreds of meters behind/above the
        // character; anchoring the heightmap/splash to the camera left the
        // character's own area (and everything ahead of it) without splashes
        // or rain surface occlusion. The Engine passes the orbit target.
        Vec3 rainAnchor = Vec3(0.0f, -1.0e9f, 0.0f);
        Vec3 windDirection;
        float time = 0.0f;
        float deltaTime = 0.016f;
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
        VkImageView depthView = VK_NULL_HANDLE;
        // Camera scene depth (gbuffer), used by the particle fragment shader
        // for the soft rain occlusion. depthView above stays as the top-down
        // heightmap depth for other consumers.
        VkImageView sceneDepthView = VK_NULL_HANDLE;
        uint32_t screenWidth = 0;
        uint32_t screenHeight = 0;
        float heightmapBlur = 0.0f;
        float cloudBaseHeight = 200.0f; // particles below this height, camera above it sees no rain
        Vec3 sunDir = Vec3(0.0f, 1.0f, 0.0f);
        // Debug: color rain particles by occlusion fate (DoF-style debug view).
        float debugRainOcclusion = 0.0f;
        float debugRainRegions = 0.0f;
        // Debug: splash shaders render big opaque red quads (splash area viz).
        float debugSplashArea = 0.0f;
    };

    bool init(VulkanContext* ctx);
    void shutdown();

    // Set the 2D cloud coverage map used to mask precipitation spawn areas.
    void setCloudCoverageMap(const CloudCoverageNoise* map) { m_cloudCoverageMap = map; }
    void setCloudCoverageTexture(VkImageView view, VkSampler sampler) {
        m_cloudCoverageView = view;
        m_cloudCoverageSampler = sampler;
    }
    void setCloudCoverageParams(float layer, float threshold, float coverage, const Vec2& windOffset) {
        m_cloudLayer = layer;
        m_cloudThreshold = threshold;
        m_cloudCoverage = coverage;
        m_cloudWindOffset = windOffset;
    }

    // Set the 2D cloud altitude map (per-pixel cloud center height) used to
    // kill precipitation that rises above the local cloud base.
    void setCloudAltitudeTexture(VkImageView view, VkSampler sampler) {
        m_cloudAltitudeView = view;
        m_cloudAltitudeSampler = sampler;
    }
    void setCloudAltitudeRange(float cloudBottom, float cloudTop) {
        m_cloudBottom = cloudBottom;
        m_cloudTop = cloudTop;
    }
    void setCloudScale(float scale) { m_cloudScale = scale; }

    // Renderiza partículas 3D de chuva/neve sobre o inputColor
    void render(VkCommandBuffer cmd,
                VkImageView inputColor,
                VkImageView outputColor,
                uint32_t width, uint32_t height,
                const WeatherParams& weather,
                const RenderParams& params);

    // Build/clear the top-down heightmap from the top-down orthographic depth
    // pass. Called by PostProcessor before the weather overlay pass so the
    // overlay and particle shaders can both read the same heightmap.
    void updateHeightmapOnly(VkCommandBuffer cmd, VkImageView depthView, const RenderParams& params);

    // Returns the blurred heightmap written this frame.  Valid after updateHeightmapOnly().
    VkImageView heightmapView() const { return m_heightmapBlurView; }
    VkSampler heightmapSampler() const { return m_heightmapSampler; }

    // Top-down orthographic depth pass (always looks straight down).
    void renderTopDownDepth(VkCommandBuffer cmd,
                            const Vec3& cameraPos,
                            TerrainRenderer& terrain,
                            ModelRenderer& models,
                            SpriteRenderer& sprites,
                            VkPipeline shadowPipeline,
                            VkPipelineLayout shadowLayout,
                            BindlessDescriptor* bindless,
                            const FrameUBO& frameUbo,
                            VkBuffer spriteInstanceBuffer,
                            uint32_t spriteCount);

    VkImageView topDownDepthView() const { return m_topDownDepthView; }

    void setTier(WeatherTier tier) { m_tier = tier; }
    WeatherTier tier() const { return m_tier; }

    void setCloudBaseHeight(float height) { m_cloudBaseHeight = height; }
    float cloudBaseHeight() const { return m_cloudBaseHeight; }

    // Direction the fixed rain box drifts in (X/Z plane, normalized internally).
    void setRainBoxMoveDirection(const Vec3& dir) {
        Vec2 d(dir.x, dir.z);
        float len = glm::length(d);
        m_rainBoxMoveDir = (len > 0.001f) ? (d / len) : Vec2(1.0f, 0.0f);
    }

    // Fixed world-space center of the precipitation box (test), drifting
    // using the cloud layer wind speed directly.
    Vec3 rainBoxCenterBase() const { return Vec3(750.0f, 74.0f, 900.0f); }
    Vec3 rainBoxCenter() const { return rainBoxCenterBase() + Vec3(m_rainBoxOffset.x, 0.0f, m_rainBoxOffset.y); }

    // Move the rain box using the cloud layer wind speed directly.
    void updateRainBoxOffset(float dt, float cloudSpeed) {
        float windSpeed = cloudSpeed * 10.0f;
        m_rainBoxOffset += m_rainBoxMoveDir * windSpeed * dt;
    }

    // Extra rain boxes that get their own particle draw each frame, e.g. one
    // per independent cloud so the rain follows the cloud's ground shadow.
    // Updated every frame by the caller.
    FastArena m_arena{32 * 1024 * 1024}; // 32MB bump allocator

    struct RainFollower {
        Vec3 boxCenter; // x,z = rain box center (cloud's ground shadow), y = cloud plane height
        Vec4 occluder;  // x,y = cloud blob center XZ, z = blob occluder radius (m, early-out)
        Vec4 covBounds; // x,y = coverage world min XZ, z,w = coverage world size XZ (0 = no coverage)
        Vec2 covWind;   // coverage wind offset (UV), matches the visual cloud drift
        // World-space size of this follower's rain box in XZ (full extents, m).
        // Matches the owning cloud's baked footprint (radius/falloff per axis)
        // so drops spread under the WHOLE cloud instead of clustering in the
        // legacy 80x80 m test box (which also blew out additively into a
        // glowing blob). Carried to the vertex shader via cloudRange.yw.
        Vec2 boxSizeXZ = Vec2(80.0f);
        // The owning cloud's own coverage array: the particle shader samples
        // it at the ray/plane crossing so the occlusion matches the exact
        // rendered silhouette instead of an analytic disc.
        VkImageView coverageView = VK_NULL_HANDLE;
        VkSampler coverageSampler = VK_NULL_HANDLE;
        // Per-cloud rain intensity multiplier derived from how "loaded" the
        // cloud is (density -> rain/storm darkness). Scales each drop's alpha
        // so a small but heavy cloud pours harder WITHOUT spawning more
        // particles (the cheap, FPS-friendly lever; the shared particle pool
        // cannot be resized per cloud anyway).
        float rainRate = 1.0f;
    };
    void setRainFollowers(const std::vector<RainFollower>& followers) { m_rainFollowers = followers; }
    const std::vector<RainFollower>& rainFollowers() const { return m_rainFollowers; }
    size_t rainFollowerCount() const { return m_rainFollowers.size(); }
    // Target (post-transition) rain intensity, used ONLY to size the particle
    // pool: the pool is built once at its final count when a storm ramps in,
    // instead of re-filling + blocking-uploading it every frame while the
    // lerped intensity crawls up (that per-frame rebuild was the post-spawn
    // frame-spike burst). Per-drop dimming still follows the lerped value,
    // so the fade-in reads identically (dense but dim drops).
    void setPoolTargetRainIntensity(float v) { m_poolTargetRainIntensity = v; }
    // Final rainy-cloud (follower) count of the field being spawned. The rain
    // pool scales with the follower count; while a field drains in (1 cloud
    // per frame) a stale small count would grow the pool every 4 followers —
    // each growth re-fills + blocking-uploads ~1M particles (multi-second
    // frames mid-transition). Size the pool for the FINAL count up front:
    // one rebuild at transition start, none during the drain.
    void setPoolTargetFollowerCount(uint32_t n) { m_poolTargetFollowerCount = n; }
    float cloudBottom() const { return m_cloudBottom; }
    float cloudTop() const { return m_cloudTop; }
    // When the procedural cloud field replaces the global cloud layer, global
    // precipitation around the camera is suppressed: rain comes from the rainy
    // field clouds (followers). Snow/debris are suppressed too while active.
    void setSuppressGlobalPrecip(bool on) { m_suppressGlobalPrecip = on; }
    bool suppressGlobalPrecip() const { return m_suppressGlobalPrecip; }

    // Live particle-pool state (F3 telemetry): composition the pool was last
    // seeded with, plus buffer capacity and splash group counts.
    uint32_t particleCount() const { return m_particleCount; }
    uint32_t particleBufferCapacity() const { return m_particleBufferCapacity; }
    uint32_t poolRainCount() const { return m_poolRainCount; }
    uint32_t poolSnowCount() const { return m_poolSnowCount; }
    uint32_t poolDustCount() const { return m_poolDustCount; }
    uint32_t splashCount() const { return m_splashCount; }
    uint32_t splashGlobalCount() const { return m_splashGlobalCount; }
    uint32_t poolTargetFollowerCount() const { return m_poolTargetFollowerCount; }
    // How exposed the camera is to follower (per-cloud) rain, 0..1. In field
    // mode the legacy fixed rain box is meaningless, so screen-space rain FX
    // (lens drops) gate on this instead: 1 when the camera XZ sits inside a
    // rainy cloud's footprint, soft 20m edge, max over followers.
    float cameraRainExposure(const Vec3& cameraPos) const;

    static constexpr uint32_t HEIGHTMAP_SIZE = 512;
    // World extent of the camera-centered top-down heightmap. 480m culled
    // ground splashes ~240m ahead of the camera — at pitch ~45° most of the
    // visible ground was splashless. 960m covers the pitched-forward view;
    // texel goes 0.94m -> 1.88m (roof/terrain rain occlusion unaffected —
    // buildings are many meters across).
    static constexpr float HEIGHTMAP_WORLD_SIZE = 960.0f;

    // Max rain followers / cross-cloud occluders. The Engine cap is
    // category-driven: every spawned rainy cloud in the Rain/Extreme group
    // rains; non-rain weathers with manual rainy clouds keep a 12-follower
    // budget.
    static constexpr uint32_t MAX_RAIN_OCCLUDERS = 100;

private:
    VulkanContext* m_ctx = nullptr;
    WeatherTier m_tier = WeatherTier::Medium;
    float m_cloudBaseHeight = 200.0f;

    VkBuffer m_particleBuffer = VK_NULL_HANDLE;
    VmaAllocation m_particleAlloc = VK_NULL_HANDLE;
    uint32_t m_particleCount = 0;
    uint32_t m_particleBufferCapacity = 0;
    // Snow slice inside the contiguous [rain | snow | dust] particle pool:
    // drawn standalone when the procedural cloud field suppresses the global
    // precipitation draw (Snow-category clouds carry no rain followers).
    uint32_t m_snowStart = 0;
    uint32_t m_snowCount = 0;
    // Dust/sand slice inside the same pool, right after snow ([rain|snow|dust]).
    uint32_t m_dustStart = 0;
    uint32_t m_dustCount = 0;
    // Composition the pool was LAST SEEDED with. The rebuild trigger compares
    // the per-frame targets against these: a zero crossing in ANY component
    // must rebuild even when the TOTAL stays inside the 5% hysteresis band —
    // otherwise a component that ramps out (e.g. sandstorm dust fading into
    // rain) leaves a stale tail in the pool that keeps drawing forever
    // (author feedback 2026-08-11: "troquei de sandstorm pra rain e manteve
    // a sandstorm somada").
    uint32_t m_poolRainCount = 0;
    uint32_t m_poolSnowCount = 0;
    uint32_t m_poolDustCount = 0;

    VkBuffer m_splashBuffer = VK_NULL_HANDLE;
    VmaAllocation m_splashAlloc = VK_NULL_HANDLE;
    uint32_t m_splashCount = 0;

    VkPipeline m_particlePipeline = VK_NULL_HANDLE;
    VkPipeline m_splashPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descSets;

    // Cross-cloud rain occlusion: UBO (binding 5) with every rainy cloud's
    // blob occluder, so a drop is hidden by ANY cloud between it and the
    // camera, not only by its owning cloud (see MAX_RAIN_OCCLUDERS above).
    VkBuffer m_rainOccBuffer = VK_NULL_HANDLE;
    VmaAllocation m_rainOccAlloc = VK_NULL_HANDLE;
    void* m_rainOccMapped = nullptr;
    float m_poolTargetRainIntensity = 0.0f; // see setPoolTargetRainIntensity
    uint32_t m_poolTargetFollowerCount = 0; // see setPoolTargetFollowerCount

    // Rain streak shape tuning UBO (binding 7): 2 vec4s filled every frame
    // from WeatherParams (shape = width/length/edge/tipFade, look =
    // alphaGain/brightness/pixelFade). Persistently mapped.
    VkBuffer m_tuneBuffer = VK_NULL_HANDLE;
    VmaAllocation m_tuneAlloc = VK_NULL_HANDLE;
    void* m_tuneMapped = nullptr;

    const CloudCoverageNoise* m_cloudCoverageMap = nullptr;
    VkImageView m_cloudCoverageView = VK_NULL_HANDLE;
    VkSampler m_cloudCoverageSampler = VK_NULL_HANDLE;
    float m_cloudLayer = 0.0f;
    float m_cloudThreshold = 0.0f;
    float m_cloudCoverage = 1.0f;
    Vec2 m_cloudWindOffset = Vec2(0.0f);

    VkImageView m_cloudAltitudeView = VK_NULL_HANDLE;
    VkSampler m_cloudAltitudeSampler = VK_NULL_HANDLE;
    float m_cloudBottom = 500.0f;
    float m_cloudTop = 1300.0f;
    float m_cloudScale = 1.5f;

    // Fixed rain box movement (direction + accumulated offset).
    Vec2 m_rainBoxOffset = Vec2(0.0f);
    Vec2 m_rainBoxMoveDir = Vec2(1.0f, 0.0f);

    // Per-cloud rain boxes (see setRainFollowers) and their rain intensity
    // when the global weather has none (e.g. Clear). m_followerRainIntensity
    // only DIMS each follower drop (push.params.y): the particle pipeline is
    // drop into a fat shimmering white dot; 0.25 was invisible in stills.
    // 0.5 sits in between now that the shimmer itself is fixed by the
    // blurred rain mask + smoother coverage. Pool density does NOT scale
    // with this anymore (decoupled).
    std::vector<RainFollower> m_rainFollowers;
    float m_followerRainIntensity = 0.5f;
    bool m_suppressGlobalPrecip = false;

    // One descriptor set per follower: same as m_descSets[0] but binding 3
    // points at the follower's own coverage array. Allocated on demand from
    // m_descPool and rewritten every frame inside render().
    std::vector<VkDescriptorSet> m_followerDescSets;

    // Fullscreen rain-occlusion debug map (DoF-visualizeCoC style): dims the
    // scene and tints it by the per-follower occlusion field. Uses the same
    // descriptor layout (bindings 1=scene depth, 3=coverage).
    VkPipeline m_debugMapPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_debugMapLayout = VK_NULL_HANDLE;

    // Camera-relative rain box: recreate particles when the camera moves enough.
    Vec3 m_lastRainCameraPos = Vec3(1e9f);
    float m_rainRebuildTimer = 0.0f;
    static constexpr float RAIN_REBUILD_DISTANCE = 4.0f;
    static constexpr float RAIN_REBUILD_INTERVAL = 0.25f;

    // Throttle the expensive top-down depth pass (used for rain heightmap).
    // 8 frames keeps rain occlusion responsive while halving the geometry cost
    // versus the old 4-frame interval.
    uint32_t m_topDownFrameCounter = 0;
    static constexpr uint32_t TOPDOWN_DEPTH_INTERVAL = 8;

    // Throttle splash rebuilds; they only need to follow the camera slowly.
    float m_splashTimer = 0.0f;
    static constexpr float SPLASH_REBUILD_INTERVAL = 0.2f;

    // Splash buffer layout: [global group around camera | one group per rain
    // follower inside its cloud footprint]. Groups are (firstInstance, count),
    // rebuilt together with the splash particles so ground splashes appear
    // under each independent cloud even in Clear weather.
    uint32_t m_splashGlobalCount = 0;
    std::vector<std::pair<uint32_t, uint32_t>> m_splashGroups;
    // Last uploaded per-cloud splash counts — rebuild only when this
    // distribution changes (weights are static per cloud; see
    // updateSplashesForWeather).
    std::vector<uint32_t> m_splashCloudCounts;

    VkSampler m_linearSampler = VK_NULL_HANDLE;

    // Single top-down heightmap (temporal ping-pong removed).
    // Raw heights are written to m_heightmapImage; a 3x3 blur is applied into
    // m_heightmapBlurImage, which is what shaders sample.
    VkImage m_heightmapImage = VK_NULL_HANDLE;
    VmaAllocation m_heightmapAlloc = VK_NULL_HANDLE;
    VkImageView m_heightmapView = VK_NULL_HANDLE;
    VkImage m_heightmapBlurImage = VK_NULL_HANDLE;
    VmaAllocation m_heightmapBlurAlloc = VK_NULL_HANDLE;
    VkImageView m_heightmapBlurView = VK_NULL_HANDLE;
    VkSampler m_heightmapSampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_heightmapDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_heightmapDescPool = VK_NULL_HANDLE;
    VkDescriptorSet m_heightmapUpdateDescSet = VK_NULL_HANDLE;
    // Blur em ida e volta: [0] heightmap -> blur, [1] blur -> heightmap.
    // Escritos uma vez (e de novo so' se as views mudarem), nunca durante a
    // gravacao de um frame.
    VkDescriptorSetLayout m_heightmapBlurDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_heightmapBlurPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_heightmapBlurDescSets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView m_heightmapBlurSetViews[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineLayout m_heightmapPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_heightmapUpdatePipeline = VK_NULL_HANDLE;
    VkPipeline m_heightmapBlurPipeline = VK_NULL_HANDLE;

    // Top-down orthographic depth target for rain heightmap generation.
    VkImage m_topDownDepthImage = VK_NULL_HANDLE;
    VmaAllocation m_topDownDepthAlloc = VK_NULL_HANDLE;
    VkImageView m_topDownDepthView = VK_NULL_HANDLE;

    bool createParticleBuffer();
    bool createSplashBuffer();
    bool createPipelines();
    bool createDescriptors();
    bool createHeightmapResources();
    void destroyHeightmapResources();
    bool createHeightmapPipeline();
    bool createTopDownDepthResources();
    void destroyTopDownDepthResources();
    void updateHeightmap(VkCommandBuffer cmd, VkImageView depthView, const RenderParams& params);

    void updateParticlesForWeather(const WeatherParams& weather, const Vec3& cameraPos, const Mat4& invViewProj, float dt, bool debugRainRegions = false);
    void updateSplashesForWeather(const WeatherParams& weather, const Vec3& cameraPos, const Vec3& rainAnchor, float dt);
};

} // namespace eruption
