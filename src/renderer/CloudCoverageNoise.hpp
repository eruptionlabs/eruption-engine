#pragma once

#include "math/Types.hpp"
#include "renderer/LocalCloud.hpp"
#include <glm/glm.hpp>
#include <vector>

namespace eruption {

// CPU-side 3D-sliced noise data for cloud coverage. This class has no Vulkan
// dependency so it can be used by CPU tests and by the CloudFluffSystem.
// Mirrors the GPU coverage_gen.comp field: the noise is 3D value noise and
// each cloud/layer samples a fixed Z slice; the CPU mirror uses layer 0's
// slice (NOISE_Z) so CPU queries match the lowest GPU layer.
class CloudCoverageNoise {
public:
    static constexpr uint32_t MAX_OCTAVES = 5;
    // Fixed Z slice of the 3D field (matches layerNoiseZ(0) in coverage_gen.comp).
    static constexpr float NOISE_Z = 2.17f;

    CloudCoverageNoise() = default;
    virtual ~CloudCoverageNoise() = default;

    void setNoiseParams(uint32_t octaves, float coverage, uint32_t seed) {
        m_octaves = glm::clamp(octaves, 0u, MAX_OCTAVES);
        m_coverage = glm::clamp(coverage, 0.0f, 1.0f);
        m_seed = seed;
    }

    void setWorldBounds(const Vec3& worldMin, const Vec3& worldMax) {
        m_worldMin = worldMin;
        m_worldMax = worldMax;
    }

    void setWindOffset(const Vec2& offset) { m_windOffset = offset; }

    uint32_t octaves() const { return m_octaves; }
    float coverage() const { return m_coverage; }
    uint32_t seed() const { return m_seed; }
    bool enabled() const { return m_octaves > 0; }

    Vec3 worldMin() const { return m_worldMin; }
    Vec3 worldMax() const { return m_worldMax; }

    // Local cloud patches (CPU-side mirror of GPU local clouds).
    void addLocalCloud(const LocalCloud& cloud) {
        if (m_localClouds.size() < LocalCloud::MAX_COUNT) m_localClouds.push_back(cloud);
    }
    void clearLocalClouds() { m_localClouds.clear(); }
    const std::vector<LocalCloud>& localClouds() const { return m_localClouds; }
    std::vector<LocalCloud>& localClouds() { return m_localClouds; }

    // Move all local clouds with the wind.
    void updateLocalClouds(float dt, const Vec3& windDirection, float windSpeed) {
        Vec2 windXZ = glm::normalize(Vec2(windDirection.x, windDirection.z));
        if (glm::length(windXZ) < 0.001f) windXZ = Vec2(1.0f, 0.0f);
        Vec2 offset = windXZ * windSpeed * dt * 10.0f;
        for (auto& c : m_localClouds) c.center += offset;
    }

    // Raw noise value at a world XZ position (0 = clear, 1 = cloud).
    // Does not apply the coverage threshold, so it can be thresholded at runtime.
    float sampleRaw(float worldX, float worldZ) const {
        if (!enabled()) return 1.0f;

        float rangeX = m_worldMax.x - m_worldMin.x;
        float rangeZ = m_worldMax.z - m_worldMin.z;
        if (rangeX <= 0.0f || rangeZ <= 0.0f) return 1.0f;

        float u = (worldX - m_worldMin.x) / rangeX + m_windOffset.x;
        float v = (worldZ - m_worldMin.z) / rangeZ + m_windOffset.y;
        u = glm::fract(u);
        v = glm::fract(v);

        const float frequency = 32.0f; // scaled so cloud detail keeps the same world-space size with the larger domain
        float value = fbm3D(u * frequency, v * frequency, NOISE_Z, m_seed, m_octaves, 0.5f, 2.0f, frequency);
        return glm::clamp(value, 0.0f, 1.0f);
    }

    // Ellipse density for a single local cloud at a world XZ position.
    static float localCloudDensity(const LocalCloud& c, float worldX, float worldZ) {
        Vec2 p(worldX - c.center.x, worldZ - c.center.y);
        float cosR = glm::cos(-c.rotation);
        float sinR = glm::sin(-c.rotation);
        Vec2 rp(p.x * cosR - p.y * sinR, p.x * sinR + p.y * cosR);
        if (c.radius.x <= 0.0f || c.radius.y <= 0.0f) return 0.0f;
        float d = glm::length(Vec2(rp.x / c.radius.x, rp.y / c.radius.y));
        float falloff = glm::max(c.falloff, 0.01f);
        float v = 1.0f - glm::smoothstep(0.0f, 1.0f, d * falloff);
        // coverage acts like the global cloud threshold: higher = fuller cloud.
        return v * c.density * (0.1f + 0.9f * c.coverage);
    }

    // Maximum local density from all spawned local clouds.
    float sampleLocal(float worldX, float worldZ) const {
        float maxDensity = 0.0f;
        for (const auto& c : m_localClouds) {
            if (!c.alive) continue;
            maxDensity = glm::max(maxDensity, localCloudDensity(c, worldX, worldZ));
        }
        return maxDensity;
    }

    // Generate a coverage value at a world XZ position (0 = clear, 1 = cloud).
    float sample(float worldX, float worldZ) const {
        float value = sampleRaw(worldX, worldZ);
        float local = sampleLocal(worldX, worldZ);
        value = glm::max(value, local);
        float threshold = 1.0f - m_coverage;
        value = glm::smoothstep(threshold, threshold + 0.25f, value);
        return glm::clamp(value, 0.0f, 1.0f);
    }

    // Fill a byte buffer with coverage values over the world bounds.
    void generate(uint32_t size, std::vector<uint8_t>& outData) const {
        outData.resize(static_cast<size_t>(size) * size);
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                float wx = m_worldMin.x + (m_worldMax.x - m_worldMin.x) * (x + 0.5f) / size;
                float wz = m_worldMin.z + (m_worldMax.z - m_worldMin.z) * (y + 0.5f) / size;
                float value = sample(wx, wz);
                outData[y * size + x] = static_cast<uint8_t>(value * 255.0f);
            }
        }
    }

protected:
    Vec3 m_worldMin = Vec3(-12000.0f, 0.0f, -12000.0f);
    Vec3 m_worldMax = Vec3(12000.0f, 2000.0f, 12000.0f);
    Vec2 m_windOffset = Vec2(0.0f);
    uint32_t m_octaves = 3;
    float m_coverage = 0.5f;
    uint32_t m_seed = 12345u;
    std::vector<LocalCloud> m_localClouds;

private:
    static int positiveMod(int v, int m) {
        int r = v % m;
        return r < 0 ? r + m : r;
    }

    // Tileable 3D value noise: the hash wraps around integer cells so that the
    // domain [0, period] matches [period, 2*period] seamlessly. Callers pass a
    // fixed Z per cloud/layer, so each one reads its own slice of the field.
    static float valueNoise3D(float x, float y, float z, uint32_t seed, float period) {
        auto hash = [](int ix, int iy, int iz, uint32_t seed) -> float {
            uint32_t n = static_cast<uint32_t>(ix * 374761393u + iy * 668265263u + iz * 982451653u + seed * 1013904223u);
            n = (n ^ (n >> 13u)) * 1274126177u;
            return static_cast<float>(n) / static_cast<float>(0xFFFFFFFFu);
        };

        int ix = static_cast<int>(glm::floor(x));
        int iy = static_cast<int>(glm::floor(y));
        int iz = static_cast<int>(glm::floor(z));
        float fx = glm::fract(x);
        float fy = glm::fract(y);
        float fz = glm::fract(z);

        int periodCells = static_cast<int>(glm::ceil(period));
        if (periodCells < 1) periodCells = 1;

        int ix0 = positiveMod(ix, periodCells);
        int iy0 = positiveMod(iy, periodCells);
        int iz0 = positiveMod(iz, periodCells);
        int ix1 = positiveMod(ix + 1, periodCells);
        int iy1 = positiveMod(iy + 1, periodCells);
        int iz1 = positiveMod(iz + 1, periodCells);

        float a = hash(ix0, iy0, iz0, seed);
        float b = hash(ix1, iy0, iz0, seed);
        float c = hash(ix0, iy1, iz0, seed);
        float d = hash(ix1, iy1, iz0, seed);
        float e = hash(ix0, iy0, iz1, seed);
        float f = hash(ix1, iy0, iz1, seed);
        float g = hash(ix0, iy1, iz1, seed);
        float h = hash(ix1, iy1, iz1, seed);

        fx = fx * fx * (3.0f - 2.0f * fx);
        fy = fy * fy * (3.0f - 2.0f * fy);
        fz = fz * fz * (3.0f - 2.0f * fz);

        return glm::mix(glm::mix(glm::mix(a, b, fx), glm::mix(c, d, fx), fy),
                        glm::mix(glm::mix(e, f, fx), glm::mix(g, h, fx), fy), fz);
    }

    static float fbm3D(float x, float y, float z, uint32_t seed, uint32_t octaves,
                       float persistence, float lacunarity, float basePeriod) {
        float total = 0.0f;
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float maxValue = 0.0f;
        for (uint32_t i = 0; i < octaves; ++i) {
            total += valueNoise3D(x * frequency, y * frequency, z * frequency, seed + i * 131u, basePeriod * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }
        return total / maxValue;
    }
};

} // namespace eruption
