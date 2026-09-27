// Clima e nuvens independentes do Engine: geracao do campo de nuvem por tipo
// de clima, nascimento/limpeza de nuvem independente, aplicacao de tipo de
// clima e carga do clima regional do mapa. Saiu do Engine.cpp em 2026-09-04,
// na quebra do arquivo.
//
// O namespace anonimo no topo veio junto: sao helpers de cobertura/ruido de
// nuvem que so' o nascimento de nuvem independente usa.

#include "core/Engine.hpp"
#include "core/Logger.hpp"
#include "renderer/WeatherTypes.hpp"
#include "formats/MapLoader.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace eruption {

// CPU replica of the local-cloud coverage formula in coverage_gen.comp, used to
// measure a spawned cloud's occupied area (pixels above threshold) for its weight.
namespace {
int cloudPositiveMod(int v, int m) { int r = v % m; return r < 0 ? r + m : r; }

float cloudHash2D(int ix, int iy, uint32_t seed) {
    uint32_t n = static_cast<uint32_t>(ix) * 374761393u
               + static_cast<uint32_t>(iy) * 668265263u
               + seed * 1013904223u;
    n = (n ^ (n >> 13u)) * 1274126177u;
    return static_cast<float>(n) / static_cast<float>(0xFFFFFFFFu);
}

float cloudValueNoise2D(float x, float y, uint32_t seed, float period) {
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    float fx = x - std::floor(x);
    float fy = y - std::floor(y);
    int pc = std::max(static_cast<int>(std::ceil(period)), 1);
    int ix0 = cloudPositiveMod(ix, pc), iy0 = cloudPositiveMod(iy, pc);
    int ix1 = cloudPositiveMod(ix + 1, pc), iy1 = cloudPositiveMod(iy + 1, pc);
    float a = cloudHash2D(ix0, iy0, seed), b = cloudHash2D(ix1, iy0, seed);
    float c = cloudHash2D(ix0, iy1, seed), d = cloudHash2D(ix1, iy1, seed);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    return glm::mix(glm::mix(a, b, fx), glm::mix(c, d, fx), fy);
}

float cloudFbm(float x, float y, uint32_t seed, int octaves, float basePeriod) {
    float total = 0.0f, amplitude = 1.0f, frequency = 1.0f, maxValue = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        total += cloudValueNoise2D(x * frequency, y * frequency, seed + static_cast<uint32_t>(i) * 131u,
                                   basePeriod * frequency) * amplitude;
        maxValue += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return total / maxValue;
}

// value baked into the coverage texture for one local cloud (see coverage_gen.comp)
float localCloudCoverageAt(Vec2 worldXZ, const LocalCloud& c, uint32_t seed) {
    Vec2 p = worldXZ - c.center;
    float cr = std::cos(-c.rotation), sr = std::sin(-c.rotation);
    Vec2 rp(p.x * cr - p.y * sr, p.x * sr + p.y * cr);
    if (c.radius.x <= 0.0f || c.radius.y <= 0.0f) return 0.0f;
    float d = glm::length(Vec2(rp.x / c.radius.x, rp.y / c.radius.y));
    // Early-out: outside the falloff envelope the smoothstep saturates and the
    // result is 0 regardless of the noise — skips the fbm for ~all far samples
    // (this runs on a 256x256 grid per spawned cloud).
    if (d * std::max(c.falloff, 0.01f) >= 1.0f) return 0.0f;
    float v = 1.0f - glm::smoothstep(0.0f, 1.0f, d * std::max(c.falloff, 0.01f));
    // Mirror of localCloudDensity (coverage_gen.comp): scale 0.02 + period
    // 4096 = big non-repeating noise features (see the shader comment).
    float noise = cloudFbm(worldXZ.x * 0.02f + c.center.x, worldXZ.y * 0.02f + c.center.y,
                           seed + 777u, 3, 4096.0f);
    float threshold = 1.0f - c.coverage;
    noise = glm::smoothstep(threshold, threshold + 0.35f, noise);
    return v * c.density * noise;
}

// Fraction of the world covered by the cloud(s) above the render threshold.
// Takes the composite blob list: coverage at a point is the max over blobs,
// mirroring sampleLocalCoverage in coverage_gen.comp.
float measureCloudAreaFraction(const std::vector<LocalCloud>& blobs, uint32_t seed, Vec3 wmin, Vec3 wmax) {
    // Cheap analytic + sparse-sample estimate: exact 256^2 CPU sampling was
    // dominating spawn cost (~25 ms/cloud). The area fraction is only used
    // to drive drift weight, so a coarse estimate is enough.
    constexpr float kThreshold = 0.05f;
    float worldArea = std::max((wmax.x - wmin.x) * (wmax.z - wmin.z), 1.0f);
    float coveredArea = 0.0f;
    for (const LocalCloud& c : blobs) {
        // Ellipse area = pi * rx * rz, scaled by density as a coverage proxy.
        const float rx = c.radius.x;
        const float rz = c.radius.y;
        coveredArea += 3.14159265f * rx * rz * (0.25f + 0.75f * c.density);
    }
    // Sparse sanity samples (32^2) to avoid overestimating overlap.
    constexpr int N = 32;
    int above = 0;
    for (int j = 0; j < N; ++j) {
        for (int i = 0; i < N; ++i) {
            Vec2 xz(wmin.x + (wmax.x - wmin.x) * (static_cast<float>(i) + 0.5f) / N,
                    wmin.z + (wmax.z - wmin.z) * (static_cast<float>(j) + 0.5f) / N);
            float cov = 0.0f;
            for (const LocalCloud& c : blobs)
                cov = std::max(cov, localCloudCoverageAt(xz, c, seed));
            if (cov > kThreshold) ++above;
        }
    }
    float sampleFrac = static_cast<float>(above) / static_cast<float>(N * N);
    float analyticFrac = std::min(coveredArea / worldArea, 1.0f);
    return std::min(sampleFrac, analyticFrac);
}
} // namespace

float Engine::independentCloudPlaneY(float altitude) const {
    altitude = glm::clamp(altitude, 0.0f, 1.0f);
    if (m_cloudBoxOverride)
        return m_cloudBoxBottom + altitude * m_cloudBoxThickness;
    // Independent clouds live near the player, NOT in the global sky box
    // (cloudBottom=5000 would put them kilometers above the camera: only
    // their ground shadow was visible, the cloud itself rendered off-screen).
    // alt 0 ~= just above the player, alt 1 = 780m up: low clouds hug the
    // gameplay camera while high clouds read as a real sky layer. (x1.3 over
    // the original 30+alt*600 mapping — author request: field clouds sat too
    // low, and the lower they are the bigger the share of each rain box that
    // dies under roof/terrain occlusion.)
    return m_camera.target().y + 39.0f + altitude * 780.0f;
}

void Engine::spawnIndependentCloud(float altitude, float density, Vec2 jitterXZ, bool hasRain,
                                   float radiusScale, bool fromField) {
    altitude = glm::clamp(altitude, 0.0f, 1.0f);
    if (density < 0.0f) density = m_localCloudDensity;
    density = glm::clamp(density, 0.0f, 1.0f);
    if (m_independentCloudCount >= LocalCloud::MAX_COUNT) {
        ERUPTION_LOG_WARN("Independent cloud limit reached (%u)", LocalCloud::MAX_COUNT);
        return;
    }

    // Spawn at the camera orbit target (player position) plus an optional XZ
    // jitter (used to spread stacked spawns), altitude = slider.
    Vec3 pos = m_camera.target();
    const Vec2 spawnCenter(pos.x + jitterXZ.x, pos.z + jitterXZ.y);
    const Vec2 baseRadius(m_localCloudRadiusX, m_localCloudRadiusZ);
    auto makeBlob = [&](Vec2 c, Vec2 r, float dens, float rot) {
        LocalCloud lc;
        lc.center = c;
        lc.radius = r;
        lc.density = dens;
        lc.falloff = m_localCloudFalloff;
        lc.altitude = altitude;
        lc.rotation = rot;
        lc.coverage = m_localCloudCoverage;
        lc.layerMask = 1;
        return lc;
    };
    std::vector<LocalCloud> blobs;
    if (radiusScale >= 3.0f) {
        // Composite cloud: a single huge blob bakes as a regular grid of
        // puffs — the internal coverage fbm repeats every 80 m in world
        // space, and a wide envelope makes the lattice obvious. Splitting the
        // footprint into overlapping sub-blobs at jittered offsets
        // decorrelates the noise phase per blob, so the merged silhouette
        // reads as ONE organic cloud. All blobs live in the same layer:
        // one drift, one occluder, one rain box.
        std::mt19937 rng(0xC10CDu + m_independentCloudCount * 2654435761u);
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        blobs.push_back(makeBlob(spawnCenter, baseRadius * (radiusScale * 0.6f),
                                 density, m_localCloudRotation));
        constexpr int K = 5;
        for (int i = 0; i < K; ++i) {
            const float ang = (static_cast<float>(i) + 0.35f * u01(rng)) * (6.2831853f / static_cast<float>(K));
            const float dist = (0.38f + 0.22f * u01(rng)) * radiusScale * m_localCloudRadiusX;
            const Vec2 off(std::cos(ang) * dist,
                           std::sin(ang) * dist * (baseRadius.y / baseRadius.x));
            const float rs = radiusScale * (0.28f + 0.14f * u01(rng));
            blobs.push_back(makeBlob(spawnCenter + off, baseRadius * rs,
                                     density * (0.7f + 0.3f * u01(rng)),
                                     m_localCloudRotation + (u01(rng) - 0.5f) * 1.2f));
        }
    } else {
        blobs.push_back(makeBlob(spawnCenter, baseRadius * radiusScale,
                                 density, m_localCloudRotation));
    }

    // Merge ONLY into a layer at the same altitude AND the same XZ: each layer
    // drifts with its own weight, so clouds at different positions must keep
    // their own layer to react independently to the wind.
    for (auto& layer : m_independentCloudLayers) {
        if (std::abs(layer.altitude - altitude) < 1e-3f &&
            glm::distance(layer.centerXZ, spawnCenter) < 1e-3f) {
            for (const LocalCloud& b : blobs) layer.array->addLocalCloud(b);
            ++m_independentCloudCount;
            ERUPTION_LOG_WARN("Independent cloud MERGED into layer alt=%.2f (total=%u, layers=%zu)",
                            altitude, m_independentCloudCount, m_independentCloudLayers.size());
            return;
        }
    }

    // New layer: its own coverage array (same class as the global cloud layer),
    // no procedural noise (octaves=0), no tiling (CLAMP_TO_BORDER sampler).
    auto& global = m_cloudLayerRenderer.coverageArray();
    // Non-field clouds (classic debug + manual UI spawns) use the commit-era
    // altitude mapping (8 + alt*120 above the player) so they stay low and
    // readable with the legacy volumetric dome; the weather field keeps the
    // high sky mapping (30 + alt*600).
    const float planeY = fromField ? independentCloudPlaneY(altitude)
                                   : (m_camera.target().y + 8.0f + altitude * 120.0f);
    // Keep the coverage array's vertical box consistent with the plane Y:
    // with the override use it verbatim, otherwise use the local box
    // anchored near the player (same mapping as the plane Y above).
    const float altSpan = fromField ? 400.0f : 120.0f;
    const float boxBottom = m_cloudBoxOverride ? m_cloudBoxBottom : (planeY - altitude * altSpan);
    const float boxThickness = m_cloudBoxOverride ? m_cloudBoxThickness : altSpan;
    // Spawn-phase timing (perf investigation): each phase is a blocking
    // immediateSubmit or heavy CPU loop — log the breakdown per spawn.
    const auto spawnPhaseT0 = std::chrono::steady_clock::now();
    auto phaseMs = [&]() {
        const float ms = std::chrono::duration<float, std::milli>(
                             std::chrono::steady_clock::now() - spawnPhaseT0).count();
        return ms;
    };
    float msInit = 0.0f, msAdd = 0.0f, msBake = 0.0f, msReg = 0.0f, msMeasure = 0.0f;
    auto array = std::make_unique<CloudCoverageArray>();
    uint32_t seed = 20260711u + static_cast<uint32_t>(m_independentCloudLayers.size()) * 131u;
    // Independent layers are local overlays: use a much smaller coverage
    // resolution than the global field. 128^2 is enough for a single storm
    // cell and cuts the compute bake and ray-march cost by ~16x.
    if (!array->init(m_cloudLayerRenderer.context(), 1, 128,
                     global.worldMin(), global.worldMax(),
                     0, 0.0f, seed,
                     boxBottom, boxThickness,
                     VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)) {
        ERUPTION_LOG_ERROR("Failed to create independent cloud layer");
        return;
    }
    msInit = phaseMs();
    for (const LocalCloud& b : blobs) array->addLocalCloud(b);
    msAdd = phaseMs() - msInit;
    // init() generated an empty texture (the cloud was added after); regenerate
    // now so the cloud is baked in immediately.
    CloudCoverageArray* raw = array.get();
    m_cloudLayerRenderer.context()->immediateSubmit([&](VkCommandBuffer cmd) {
        raw->recordComputeGeneration(cmd);
    });
    msBake = phaseMs() - msInit - msAdd;

    uint32_t id = m_cloudLayerRenderer.registerCloudLayer(raw);
    if (id == UINT32_MAX) {
        ERUPTION_LOG_ERROR("Failed to register independent cloud layer");
        return;
    }

    // Color by purpose: rain clouds are dark and storm-tinted (the denser the
    // darker); dry clouds stay almost white regardless of density.
    const float rain = hasRain ? (0.3f + 0.65f * density) : 0.15f * density;
    const float storm = hasRain ? (0.15f + 0.6f * density) : 0.05f * density;
    m_cloudLayerRenderer.setCloudLayerWeather(id, rain, storm);
    msReg = phaseMs() - msInit - msAdd - msBake;

    // Weight = density + occupied area (coverage pixels above threshold) +
    // rain darkness. Heavier clouds drift slower (see the wind update).
    const float areaFrac = measureCloudAreaFraction(blobs, seed, global.worldMin(), global.worldMax());
    msMeasure = phaseMs() - msInit - msAdd - msBake - msReg;
    const float darkness = std::min(0.55f * rain + 0.35f * storm, 0.75f);
    const float weight = 1.0f + 2.5f * density + 12.0f * areaFrac + 1.5f * darkness;

    m_independentCloudLayers.push_back({std::move(array), altitude, planeY,
                                        spawnCenter, density, weight, hasRain, fromField, id});
    ++m_independentCloudCount;
    ERUPTION_LOG_WARN("Independent cloud layer SPAWNED: alt=%.2f planeY=%.1f density=%.2f area=%.1f%% weight=%.2f rain=%d blobs=%zu at (%.1f, %.1f) uv=(%.2f, %.2f) world=[(%.0f,%.0f)-(%.0f,%.0f)] (total=%u, layers=%zu) spawnMs init=%.1f add=%.1f bake=%.1f reg=%.1f measure=%.1f total=%.1f",
                    altitude, planeY, density, areaFrac * 100.0f, weight, hasRain ? 1 : 0,
                    blobs.size(),
                    spawnCenter.x, spawnCenter.y,
                    (spawnCenter.x - global.worldMin().x) / (global.worldMax().x - global.worldMin().x),
                    (spawnCenter.y - global.worldMin().z) / (global.worldMax().z - global.worldMin().z),
                    global.worldMin().x, global.worldMin().z,
                    global.worldMax().x, global.worldMax().z,
                    m_independentCloudCount, m_independentCloudLayers.size(),
                    msInit, msAdd, msBake, msReg, msMeasure, phaseMs());
}

void Engine::clearIndependentClouds() {
    if (m_independentCloudLayers.empty()) return;
    m_vulkan.waitIdle();
    for (auto& layer : m_independentCloudLayers) {
        m_cloudLayerRenderer.unregisterCloudLayer(layer.rendererId);
    }
    m_independentCloudLayers.clear();
    m_cloudLayerDrawOrder.clear();
    m_independentCloudCount = 0;
    m_postProcessor.weatherRenderer().setRainFollowers({});
    ERUPTION_LOG_WARN("Independent clouds cleared");
}

void Engine::applyWeatherTypeFull(WeatherType type, float intensity) {
    // NOTE (2026-08-31, author correction): this used to also push the weather
    // type's template into CloudLayerRenderer (scale/lightness/shade/softness/
    // thickness/wind + a cloudAmount transition) - that was copied from the old
    // UI-only lambda on the theory that the CLI's simpler `--weather` path
    // (just applyType + field respawn) was missing it. It was the other way
    // around: the CLI-only behavior was the CORRECT one, and pushing the cloud
    // template made the look WORSE in both places (flat/mismatched cloud
    // layer). Reverted to the original CLI behavior for both callers - do not
    // reintroduce the CloudLayerRenderer sync here.
    m_weatherSystem.applyType(type, intensity);
    m_weatherFieldType = -1; // respawn the field for the new type
}

void Engine::setInitialWeatherType(const std::string& name) {
    applyWeatherTypeFull(weatherTypeFromName(name), 1.0f);
}

void Engine::spawnWeatherCloudField(WeatherType type) {
    // Field spec per weather category: how many clouds, how high, how heavy,
    // how big, how far they spread around the player (fraction of the map's
    // smallest dimension), and whether they rain. The user's model: clear =
    // no clouds, cloudy = clear + clouds, rain = clear + a FEW raining
    // clouds, heavy rain = clear + MANY raining clouds (the full 100).
    struct FieldSpec {
        int count;
        float altMin, altMax;
        float denMin, denMax;
        float scaleMin, scaleMax, spread;
        bool rain;
    };
    FieldSpec spec;
    switch (weatherTypeCategory(type)) {
        case WeatherCategory::Clear:    spec = {  0, 0.65f, 1.00f, 0.00f, 0.20f, 2.0f, 3.5f, 0.6f, false}; break;
        case WeatherCategory::Cloudy:   spec = {100, 0.45f, 0.85f, 0.30f, 0.55f, 2.0f, 4.0f, 0.6f, false}; break;
        case WeatherCategory::Rain:     spec = { 40, 0.25f, 0.60f, 0.60f, 0.85f, 2.5f, 4.5f, 0.6f, true }; break;
        // Snow clouds are rainy (author request 2026-08-10): snow must fall
        // UNDER each cloud (masked by its exact coverage silhouette via the
        // rain followers), not in a global camera box with no cloud occlusion.
        case WeatherCategory::Snow:     spec = {100, 0.30f, 0.70f, 0.50f, 0.70f, 2.0f, 4.0f, 0.6f, true }; break;
        case WeatherCategory::Heat:     spec = {  0, 0.70f, 1.00f, 0.00f, 0.15f, 1.5f, 2.5f, 0.6f, false}; break;
        case WeatherCategory::Fog:      spec = { 40, 0.20f, 0.50f, 0.40f, 0.60f, 2.5f, 4.5f, 0.5f, false}; break;
        case WeatherCategory::NightSky: spec = {  0, 0.80f, 1.00f, 0.10f, 0.30f, 1.5f, 3.0f, 0.6f, false}; break;
        case WeatherCategory::Extreme:  spec = {100, 0.10f, 0.45f, 0.85f, 1.00f, 3.0f, 5.0f, 0.6f, true }; break;
        default:                        spec = {  0, 0.65f, 1.00f, 0.00f, 0.20f, 2.0f, 3.5f, 0.6f, false}; break;
    }
    if (type == WeatherType::Stormy || type == WeatherType::Thunderstorm) {
        // Revert oversized optimization by user request: Thunderstorm needs a 
        // MASSIVE amount of clouds (proportional to others) to look impressive.
        spec = {150, 0.10f, 0.45f, 0.85f, 1.00f, 3.0f, 5.0f, 0.6f, true};
    }

    // Rain intensity drives cloud size: a cloud that rains a LOT is a VERY
    // BIG cloud (user model). drizzle (0.25) gets a modest bump, a storm
    // (0.85-0.95) roughly triples the footprint. Composite-cloud splitting in
    // spawnIndependentCloud (scale >= 3) keeps the silhouette organic.
    if (spec.rain) {
        const float rainI = glm::clamp(getWeatherTypeInfo(type).baseParams.rainIntensity, 0.0f, 1.0f);
        const float sizeBoost = 1.0f + 2.0f * rainI;
        spec.scaleMin *= sizeBoost;
        spec.scaleMax *= sizeBoost;
    }

    clearIndependentClouds();
    m_pendingCloudSpawns.clear();

    // ERUPTION_TEST_CLOUDS_NO_RAIN=1 (debug): spawn the field without rain followers
    // to isolate the GPU cost of the cloud layers vs the rain.
    const bool testNoRain = std::getenv("ERUPTION_TEST_CLOUDS_NO_RAIN") != nullptr;

    // Spread the clouds over a disc around the player sized by the map.
    auto& global = m_cloudLayerRenderer.coverageArray();
    const Vec3 wsz = global.worldMax() - global.worldMin();
    const float mapMin = std::max(1.0f, std::min(std::abs(wsz.x), std::abs(wsz.z)));
    const float spreadM = spec.spread * mapMin;

    // Deterministic per type: the same weather always grows the same field.
    // Spawns go through the pending queue and drain a few per frame so the
    // field appears gradually instead of hitching the frame it triggers on.
    std::mt19937 rng(0xC10Du + static_cast<uint32_t>(type) * 977u);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    // Graphics-preset budget: weak GPUs cap the field size (each cloud is a
    // raymarched draw + a shadow draw + a possible rain follower).
    const int fieldCount = std::min(spec.count, m_maxFieldClouds);
    for (int i = 0; i < fieldCount; ++i) {
        // METADE das nuvens de chuva, e UM TERCO das secas, nascem num disco
        // curto em volta do jogador. Antes so' as de chuva tinham isso e o
        // campo "cloudy" (40 nuvens espalhadas por 0,6 x o mapa = disco de
        // 2400 u no parana_field) quase nunca punha uma nuvem SOBRE a area
        // visivel - e a sombra de nuvem cai reta debaixo da nuvem, entao nao
        // havia sombra nenhuma no chao a' vista (autor 2026-09-06). Com o
        // terco perto, o ceu nublado passa a sombrear o chao que a camera ve.
        const int nearCount = spec.rain ? fieldCount / 2 : fieldCount / 3;
        const bool nearPlayer = i < nearCount;
        // Near-player rainy clouds ride HIGH in the spec's alt band: rain
        // falls cloud->ground regardless of cloud height, but a low cloud
        // (alt 0.10-0.20 -> plane ~120-200m) engulfs the orbit camera at
        // gameplay zoom (full-screen dark volume, tested pitch30/dist250).
        const float altNearMin = std::max(spec.altMin, 0.35f);
        const float alt = nearPlayer ? glm::mix(altNearMin, spec.altMax, dist01(rng))
                                     : glm::mix(spec.altMin, spec.altMax, dist01(rng));
        const float den = glm::mix(spec.denMin, spec.denMax, dist01(rng));
        // Rain fields on big maps: the wide scenic spread (0.6x mapMin, e.g.
        // 900m on vila-A) leaves the player's own column without a raining
        // cloud most of the time ("quase nao chove" em mapa grande, denso em
        // mapa pequeno tipo arena-A com 300m). Capping the whole field at
        // ~400m was tried and REJECTED: the full dome of low storm clouds
        // around the camera blocks the view of the scene. Split instead:
        // half the rainy clouds spawn in a tight disc around the player
        // (guaranteed overhead rain, arena-A-like density), the rest keep
        // the wide scenic spread.
        const float discM = nearPlayer ? std::min(spreadM, 350.0f) : spreadM;
        // Uniform disc around the player.
        const float ang = dist01(rng) * 6.2831853f;
        const float rad = discM * std::sqrt(dist01(rng));
        const float scale = glm::mix(spec.scaleMin, spec.scaleMax, dist01(rng));
        m_pendingCloudSpawns.push_back({alt, den,
                                        std::cos(ang) * rad, std::sin(ang) * rad,
                                        spec.rain && !testNoRain,
                                        scale, true});
    }
    ERUPTION_LOG_WARN("Weather cloud field: type=%u category=%d queued %d clouds (spread=%.0fm)",
                    static_cast<uint32_t>(type),
                    static_cast<int>(weatherTypeCategory(type)),
                    fieldCount, spreadM);
}

// Regional climate description for a map (G9). Optional by design: a map with
// no file keeps a single global weather state and behaves exactly as before, so
// this can be adopted one map at a time.
void Engine::loadMapClimate(const std::string& mapName) {
    m_weatherSystem.climate().clear();
    const std::string path = "data/climate/" + mapName + ".json";
    if (!m_weatherSystem.climate().loadFromFile(path)) {
        ERUPTION_LOG_INFO("[CLIMATE] %s sem descricao de clima regional; usando clima global",
                          mapName.c_str());
    }
}

} // namespace eruption
