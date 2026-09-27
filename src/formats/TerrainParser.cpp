#include "formats/TerrainParser.hpp"
#include "core/Logger.hpp"
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace eruption {

// Sanity limits to prevent memory exhaustion on encrypted/corrupted data
static constexpr int32_t MAX_TERRAIN_TEXTURES = 4096;
static constexpr int32_t MAX_TERRAIN_SURFACES = 500000;
static constexpr int32_t MAX_TERRAIN_CUBES = 1048576; // 1024x1024
static constexpr int32_t MAX_TERRAIN_LIGHTMAPS = 500000;
static constexpr int32_t MAX_TERRAIN_WATER_PLANES = 10000;

static TerrainFile parseEruptGndBin(const uint8_t* data, size_t size) {
    BinaryReader reader(data, size);
    TerrainFile terrain;

    // Magic "ERUPTGNDBIN" (11 bytes) already verified by caller.
    reader.skip(11);

    terrain.version = static_cast<uint16_t>(reader.readU32());
    terrain.width = reader.readU32();
    terrain.height = reader.readU32();
    terrain.scale = reader.readFloat();

    if (terrain.version >= 2) {
        terrain.offsetX = reader.readFloat();
        terrain.offsetZ = reader.readFloat();
    }

    if (terrain.width == 0 || terrain.height == 0 || terrain.width > 1024 || terrain.height > 1024) {
        ERUPTION_LOG_ERROR("ERUPTGNDBIN invalid dimensions %ux%u", terrain.width, terrain.height);
        terrain.width = 0;
        terrain.height = 0;
        return terrain;
    }

    uint32_t textureCount = reader.readU32();
    terrain.textures.reserve(textureCount);
    for (uint32_t i = 0; i < textureCount; ++i) {
        uint16_t len = reader.readU16();
        std::string tex = reader.readString(len);
        size_t nullPos = tex.find('\0');
        if (nullPos != std::string::npos) tex.resize(nullPos);
        terrain.textures.push_back(std::move(tex));
    }

    uint32_t surfaceCount = reader.readU32();
    terrain.surfaces.resize(surfaceCount);
    for (auto& surf : terrain.surfaces) {
        for (int j = 0; j < 4; ++j) surf.u[j] = reader.readFloat();
        for (int j = 0; j < 4; ++j) surf.v[j] = reader.readFloat();
        surf.textureId = reader.readU16();
        surf.lightmapId = reader.readU16();
        surf.color = reader.readU32();
    }

    uint32_t cubeCount = reader.readU32();
    terrain.cubes.resize(cubeCount);
    for (auto& cube : terrain.cubes) {
        for (int j = 0; j < 4; ++j) cube.height[j] = reader.readFloat();
        cube.surfaceTop = reader.readI32();
        cube.surfaceNorth = reader.readI32();
        cube.surfaceEast = reader.readI32();
    }

    if (!reader.eof()) {
        uint32_t lightmapCount = reader.readU32();
        terrain.lightmapSlices.resize(lightmapCount);
        for (auto& lm : terrain.lightmapSlices) {
            lm.shadowmap.resize(64);
            lm.lightmap_rgb.resize(192);
            for (int j = 0; j < 64; ++j) lm.shadowmap[j] = reader.readU8();
            for (int j = 0; j < 192; ++j) lm.lightmap_rgb[j] = reader.readU8();
        }
    }

    if (!reader.eof()) {
        uint32_t waterCount = reader.readU32();
        terrain.waterPlanes.resize(waterCount);
        for (auto& wp : terrain.waterPlanes) {
            wp.level = reader.readFloat();
            wp.type = reader.readU32();
            wp.waveHeight = reader.readFloat();
            wp.waveSpeed = reader.readFloat();
            wp.wavePitch = reader.readFloat();
            wp.textureCycling = reader.readU32();
        }
    }

    TerrainParser::computeSmoothNormals(terrain);
    TerrainParser::computeSmoothColors(terrain);
    return terrain;
}

TerrainFile TerrainParser::parse(const uint8_t* data, size_t size) {
    BinaryReader reader(data, size);
    TerrainFile terrain;

    if (size >= 11 && std::memcmp(data, "ERUPTGNDBIN", 11) == 0) {
        return parseEruptGndBin(data, size);
    }

    ERUPTION_LOG_ERROR("TerrainParser: unsupported terrain format (expected ERUPTGNDBIN). "
                       "Source terrain formats are no longer supported by the engine runtime.");
    return terrain;
}

void TerrainParser::computeSmoothNormals(TerrainFile& terrain) {
    uint32_t W = terrain.width;
    uint32_t H = terrain.height;
    terrain.smoothNormals.assign((W + 1) * (H + 1), Vec3(0.0f));

    float tileSize = terrain.scale > 0.0f ? terrain.scale : 10.0f;
    float hScale = 1.0f;

    for (uint32_t z = 0; z < H; z++) {
        for (uint32_t x = 0; x < W; x++) {
            const auto& cube = terrain.cubes[z * W + x];
            if (cube.surfaceTop < 0 || cube.surfaceTop >= (int32_t)terrain.surfaces.size())
                continue;

            float sx = terrain.offsetX + static_cast<float>(x) * tileSize;
            float sz = terrain.offsetZ + static_cast<float>(z) * tileSize;

            Vec3 v0(sx,            -cube.height[0] * hScale, sz);
            Vec3 v1(sx + tileSize, -cube.height[1] * hScale, sz);
            Vec3 v2(sx,            -cube.height[2] * hScale, sz + tileSize);

            Vec3 edge1 = v1 - v0;
            Vec3 edge2 = v2 - v0;
            Vec3 fn = glm::normalize(glm::cross(edge2, edge1));

            // Accumulate to 4 corners
            terrain.smoothNormals[z * (W + 1) + x] += fn;
            terrain.smoothNormals[z * (W + 1) + (x + 1)] += fn;
            terrain.smoothNormals[(z + 1) * (W + 1) + x] += fn;
            terrain.smoothNormals[(z + 1) * (W + 1) + (x + 1)] += fn;
        }
    }

    for (auto& n : terrain.smoothNormals) {
        if (glm::length(n) > 0.0001f)
            n = glm::normalize(n);
        else
            n = Vec3(0, 1, 0);
    }
}

void TerrainParser::computeSmoothColors(TerrainFile& terrain) {
    uint32_t W = terrain.width;
    uint32_t H = terrain.height;
    std::vector<glm::vec4> accum((W + 1) * (H + 1), glm::vec4(0.0f));
    std::vector<uint32_t> count((W + 1) * (H + 1), 0);

    for (uint32_t z = 0; z < H; z++) {
        for (uint32_t x = 0; x < W; x++) {
            const auto& cube = terrain.cubes[z * W + x];
            if (cube.surfaceTop < 0 || cube.surfaceTop >= (int32_t)terrain.surfaces.size())
                continue;

            const auto& surf = terrain.surfaces[cube.surfaceTop];
            glm::vec4 color(
                float((surf.color >> 0) & 0xFF) / 255.0f,
                float((surf.color >> 8) & 0xFF) / 255.0f,
                float((surf.color >> 16) & 0xFF) / 255.0f,
                float((surf.color >> 24) & 0xFF) / 255.0f
            );

            // Tiles share vertices. Accumulate color to each of the 4 corners of the tile.
            uint32_t corners[4] = {
                z * (W + 1) + x,
                z * (W + 1) + (x + 1),
                (z + 1) * (W + 1) + x,
                (z + 1) * (W + 1) + (x + 1)
            };

            for (int i = 0; i < 4; i++) {
                accum[corners[i]] += color;
                count[corners[i]]++;
            }
        }
    }

    terrain.smoothColors.resize((W + 1) * (H + 1));
    for (uint32_t i = 0; i < (W + 1) * (H + 1); i++) {
        if (count[i] > 0) {
            glm::vec4 avg = accum[i] / float(count[i]);
            uint32_t r = static_cast<uint32_t>(glm::clamp(avg.r * 255.0f, 0.0f, 255.0f));
            uint32_t g = static_cast<uint32_t>(glm::clamp(avg.g * 255.0f, 0.0f, 255.0f));
            uint32_t b = static_cast<uint32_t>(glm::clamp(avg.b * 255.0f, 0.0f, 255.0f));
            uint32_t a = 255; // Forçar AO em 1.0 para evitar contornos pretos por interpolação com furos
            terrain.smoothColors[i] = (r << 0) | (g << 8) | (b << 16) | (a << 24);
        } else {
            terrain.smoothColors[i] = 0xFFFFFFFF; // Default white
        }
    }
}

std::vector<ExtractedLight> TerrainParser::extractPointLights(const TerrainFile& terrain) {
    uint32_t W = terrain.width;
    uint32_t H = terrain.height;

    // 1. Calcular luminância média global
    double totalLum = 0.0;
    int count = 0;
    for (const auto& surf : terrain.surfaces) {
        uint8_t r = (surf.color >> 0) & 0xFF;
        uint8_t g = (surf.color >> 8) & 0xFF;
        uint8_t b = (surf.color >> 16) & 0xFF;
        totalLum += (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
        count++;
    }
    float globalMeanLum = (count > 0) ? (float)(totalLum / count) : 0.5f;
    float threshold = std::max(globalMeanLum * 1.1f, 0.95f);

    struct Candidate {
        Vec3 pos;
        Vec3 color;
        float lum;
    };
    std::vector<Candidate> candidates;

    auto isWarmColor = [](uint32_t color) {
        float r = ((color >> 0) & 0xFF) / 255.0f;
        float g = ((color >> 8) & 0xFF) / 255.0f;
        float b = ((color >> 16) & 0xFF) / 255.0f;
        float maxC = std::max({r, g, b});
        float minC = std::min({r, g, b});
        float sat = (maxC < 0.001f) ? 0.0f : (maxC - minC) / maxC;

        bool warm = (r > b * 1.1f && sat > 0.1f);
        bool brightNeutral = (maxC > 0.9f && sat < 0.1f); // Allow bright white
        return warm || brightNeutral;
    };

    float tileSize = terrain.scale > 0.0f ? terrain.scale : 10.0f;
    for (uint32_t z = 0; z < H; z++) {
        for (uint32_t x = 0; x < W; x++) {
            const auto& cube = terrain.cubes[z * W + x];
            if (cube.surfaceTop < 0 || cube.surfaceTop >= (int32_t)terrain.surfaces.size())
                continue;

            const auto& surf = terrain.surfaces[cube.surfaceTop];
            uint8_t r = (surf.color >> 0) & 0xFF;
            uint8_t g = (surf.color >> 8) & 0xFF;
            uint8_t b = (surf.color >> 16) & 0xFF;
            float lum = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;

            if (lum > threshold && isWarmColor(surf.color)) {
                Candidate c;
                c.pos = Vec3(terrain.offsetX + x * tileSize + tileSize * 0.5f,
                             -(cube.height[0] + cube.height[1] + cube.height[2] + cube.height[3]) * 0.25f,
                             terrain.offsetZ + z * tileSize + tileSize * 0.5f);
                c.color = Vec3(r / 255.0f, g / 255.0f, b / 255.0f);
                c.lum = lum;
                candidates.push_back(c);
            }
        }
    }

    // Clusterização simples (grid-based)
    std::vector<ExtractedLight> lights;
    float cellSize = 20.0f; 
    
    struct Cluster {
        std::vector<Candidate> members;
    };
    std::vector<Cluster> clusters;

    for (const auto& cand : candidates) {
        bool found = false;
        for (auto& cluster : clusters) {
            if (glm::distance(cand.pos, cluster.members[0].pos) < cellSize) {
                cluster.members.push_back(cand);
                found = true;
                break;
            }
        }
        if (!found) {
            Cluster newCluster;
            newCluster.members.push_back(cand);
            clusters.push_back(newCluster);
        }
    }

    for (const auto& cluster : clusters) {
        if (cluster.members.size() < 2) continue;

        ExtractedLight pl;
        Vec3 avgPos(0.0f);
        Vec3 avgColor(0.0f);
        float avgLum = 0.0f;
        float maxDist = 0.0f;

        for (const auto& m : cluster.members) {
            avgPos += m.pos;
            avgColor += m.color;
            avgLum += m.lum;
        }
        avgPos /= (float)cluster.members.size();
        avgColor /= (float)cluster.members.size();
        avgLum /= (float)cluster.members.size();

        for (const auto& m : cluster.members) {
            maxDist = std::max(maxDist, glm::distance(avgPos, m.pos));
        }

        pl.worldPos = avgPos;
        pl.worldPos.y -= 5.0f; 
        pl.color = avgColor;
        pl.intensity = (avgLum / globalMeanLum) * 2.0f;
        pl.radius = glm::clamp(maxDist * 1.5f, 30.0f, 200.0f);

        if (pl.intensity > 2.5f && pl.color.r > pl.color.b * 1.5f)
            pl.animType = LightAnimType::Torch;
        else if (pl.intensity > 1.8f)
            pl.animType = LightAnimType::Pulse;
        else
            pl.animType = LightAnimType::Static;

        lights.push_back(pl);
    }

    if (lights.size() > 128) {
        std::sort(lights.begin(), lights.end(), [](const auto& a, const auto& b) {
            return a.intensity > b.intensity;
        });
        lights.resize(128);
    }

    return lights;
}

std::vector<ExtractedLight> TerrainParser::extractLightProbes(const TerrainFile& terrain) {
    std::vector<ExtractedLight> probes;
    
    // Create a mapping from lightmapId to the center of the surface that uses it.
    // For simplicity, we just take the first surface that uses a lightmap.
    std::vector<Vec3> lightmapCenters(terrain.lightmapSlices.size(), Vec3(0.0f));
    std::vector<int> lightmapUseCount(terrain.lightmapSlices.size(), 0);

    float tileSize = terrain.scale > 0.0f ? terrain.scale : 10.0f;
    uint32_t W = terrain.width;
    uint32_t H = terrain.height;

    for (uint32_t z = 0; z < H; z++) {
        for (uint32_t x = 0; x < W; x++) {
            const auto& cube = terrain.cubes[z * W + x];
            if (cube.surfaceTop >= 0 && cube.surfaceTop < static_cast<int32_t>(terrain.surfaces.size())) {
                const auto& surf = terrain.surfaces[cube.surfaceTop];
                if (surf.lightmapId < terrain.lightmapSlices.size()) {
                    Vec3 center(terrain.offsetX + x * tileSize + tileSize * 0.5f,
                               -(cube.height[0] + cube.height[1] + cube.height[2] + cube.height[3]) * 0.25f,
                               terrain.offsetZ + z * tileSize + tileSize * 0.5f);
                    lightmapCenters[surf.lightmapId] += center;
                    lightmapUseCount[surf.lightmapId]++;
                }
            }
        }
    }

    for (size_t i = 0; i < terrain.lightmapSlices.size(); i++) {
        if (lightmapUseCount[i] == 0) continue;

        const auto& slice = terrain.lightmapSlices[i];
        if (slice.lightmap_rgb.size() < 192) continue;

        float r_sum = 0.0f, g_sum = 0.0f, b_sum = 0.0f;
        for (size_t p = 0; p < 64; p++) {
            r_sum += slice.lightmap_rgb[p * 3 + 0];
            g_sum += slice.lightmap_rgb[p * 3 + 1];
            b_sum += slice.lightmap_rgb[p * 3 + 2];
        }

        Vec3 avgColor(r_sum / (64.0f * 255.0f),
                      g_sum / (64.0f * 255.0f),
                      b_sum / (64.0f * 255.0f));

        if (glm::length(avgColor) > 0.05f) {
            ExtractedLight probe;
            probe.worldPos = lightmapCenters[i] / static_cast<float>(lightmapUseCount[i]);
            // Raise probe slightly above ground
            probe.worldPos.y -= 2.0f; 
            probe.color = avgColor;
            probe.intensity = glm::length(avgColor);
            probe.radius = 15.0f;
            probe.animType = LightAnimType::Static;
            probes.push_back(probe);
        }
    }

    return probes;
}

TerrainMesh TerrainParser::generateMesh(const TerrainFile& terrain,
                                    uint32_t startX, uint32_t startZ,
                                    uint32_t chunkW, uint32_t chunkH) {
    TerrainMesh mesh;
    mesh.vertices.reserve(chunkW * chunkH * 4);
    mesh.indices.reserve(chunkW * chunkH * 6);

    uint32_t W = terrain.width;
    uint32_t H = terrain.height;

    for (uint32_t z = startZ; z < startZ + chunkH && z < H; z++) {
        for (uint32_t x = startX; x < startX + chunkW && x < W; x++) {
            const auto& cube = terrain.cubes[z * W + x];

            float tileSize = terrain.scale > 0.0f ? terrain.scale : 10.0f;
            float sx = terrain.offsetX + static_cast<float>(x) * tileSize;
            float sz = terrain.offsetZ + static_cast<float>(z) * tileSize;
            float hScale = 1.0f;

            // 1. TOP FACE
            if (cube.surfaceTop >= 0 && cube.surfaceTop < static_cast<int32_t>(terrain.surfaces.size())) {
                const auto& surf = terrain.surfaces[cube.surfaceTop];
                uint32_t baseIdx = static_cast<uint32_t>(mesh.vertices.size());

                TerrainVertex v0, v1, v2, v3;
                v0.position = Vec3(sx,             -cube.height[0] * hScale, sz);
                v1.position = Vec3(sx + tileSize,  -cube.height[1] * hScale, sz);
                v2.position = Vec3(sx,             -cube.height[2] * hScale, sz + tileSize);
                v3.position = Vec3(sx + tileSize,  -cube.height[3] * hScale, sz + tileSize);

                v0.texCoord = Vec2(surf.u[0], surf.v[0]);
                v1.texCoord = Vec2(surf.u[1], surf.v[1]);
                v2.texCoord = Vec2(surf.u[2], surf.v[2]);
                v3.texCoord = Vec2(surf.u[3], surf.v[3]);

                if (!terrain.smoothNormals.empty()) {
                    v0.normal = terrain.smoothNormals[z * (W + 1) + x];
                    v1.normal = terrain.smoothNormals[z * (W + 1) + (x + 1)];
                    v2.normal = terrain.smoothNormals[(z + 1) * (W + 1) + x];
                    v3.normal = terrain.smoothNormals[(z + 1) * (W + 1) + (x + 1)];
                } else {
                    Vec3 edge1 = v1.position - v0.position;
                    Vec3 edge2 = v2.position - v0.position;
                    v0.normal = v1.normal = v2.normal = v3.normal = glm::normalize(glm::cross(edge2, edge1));
                }

                if (!terrain.smoothColors.empty()) {
                    v0.color = terrain.smoothColors[z * (W + 1) + x];
                    v1.color = terrain.smoothColors[z * (W + 1) + (x + 1)];
                    v2.color = terrain.smoothColors[(z + 1) * (W + 1) + x];
                    v3.color = terrain.smoothColors[(z + 1) * (W + 1) + (x + 1)];
                } else {
                    v0.color = v1.color = v2.color = v3.color = surf.color;
                }

                for (auto* v : {&v0, &v1, &v2, &v3}) {
                    v->texIndex = surf.textureId;
                    v->matId = 0;
                }

                mesh.vertices.push_back(v0);
                mesh.vertices.push_back(v1);
                mesh.vertices.push_back(v2);
                mesh.vertices.push_back(v3);

                mesh.indices.push_back(baseIdx + 0);
                mesh.indices.push_back(baseIdx + 1);
                mesh.indices.push_back(baseIdx + 2);
                mesh.indices.push_back(baseIdx + 1);
                mesh.indices.push_back(baseIdx + 3);
                mesh.indices.push_back(baseIdx + 2);
            }

            // 2. FRONT FACE (NORTH)
            if (cube.surfaceNorth >= 0 && cube.surfaceNorth < static_cast<int32_t>(terrain.surfaces.size()) && z + 1 < H) {
                const auto& surf = terrain.surfaces[cube.surfaceNorth];
                const auto& northCube = terrain.cubes[(z + 1) * W + x];
                uint32_t baseIdx = static_cast<uint32_t>(mesh.vertices.size());

                TerrainVertex v0, v1, v2, v3;
                v0.position = Vec3(sx,            -cube.height[2] * hScale, sz + tileSize);
                v1.position = Vec3(sx + tileSize, -cube.height[3] * hScale, sz + tileSize);
                v2.position = Vec3(sx,            -northCube.height[0] * hScale, sz + tileSize);
                v3.position = Vec3(sx + tileSize, -northCube.height[1] * hScale, sz + tileSize);

                v0.texCoord = Vec2(surf.u[0], surf.v[0]);
                v1.texCoord = Vec2(surf.u[1], surf.v[1]);
                v2.texCoord = Vec2(surf.u[2], surf.v[2]);
                v3.texCoord = Vec2(surf.u[3], surf.v[3]);

                for (auto* v : {&v0, &v1, &v2, &v3}) {
                    v->texIndex = surf.textureId;
                    v->matId = 0;
                    v->color = surf.color;
                }

                Vec3 edge1 = v1.position - v0.position;
                Vec3 edge2 = v2.position - v0.position;
                v0.normal = v1.normal = v2.normal = v3.normal = glm::normalize(glm::cross(edge2, edge1));

                mesh.vertices.push_back(v0);
                mesh.vertices.push_back(v1);
                mesh.vertices.push_back(v2);
                mesh.vertices.push_back(v3);

                mesh.indices.push_back(baseIdx + 0);
                mesh.indices.push_back(baseIdx + 1);
                mesh.indices.push_back(baseIdx + 2);
                mesh.indices.push_back(baseIdx + 1);
                mesh.indices.push_back(baseIdx + 3);
                mesh.indices.push_back(baseIdx + 2);
            }

            // 3. RIGHT FACE (EAST)
            if (cube.surfaceEast >= 0 && cube.surfaceEast < static_cast<int32_t>(terrain.surfaces.size()) && x + 1 < W) {
                const auto& surf = terrain.surfaces[cube.surfaceEast];
                const auto& eastCube = terrain.cubes[z * W + (x + 1)];
                uint32_t baseIdx = static_cast<uint32_t>(mesh.vertices.size());

                TerrainVertex v0, v1, v2, v3;
                v0.position = Vec3(sx + tileSize, -cube.height[1] * hScale, sz);
                v1.position = Vec3(sx + tileSize, -cube.height[3] * hScale, sz + tileSize);
                v2.position = Vec3(sx + tileSize, -eastCube.height[0] * hScale, sz);
                v3.position = Vec3(sx + tileSize, -eastCube.height[2] * hScale, sz + tileSize);

                v1.texCoord = Vec2(surf.u[0], surf.v[0]);
                v0.texCoord = Vec2(surf.u[1], surf.v[1]);
                v3.texCoord = Vec2(surf.u[2], surf.v[2]);
                v2.texCoord = Vec2(surf.u[3], surf.v[3]);

                for (auto* v : {&v0, &v1, &v2, &v3}) {
                    v->texIndex = surf.textureId;
                    v->matId = 0;
                    v->color = surf.color;
                }

                Vec3 edge1 = v1.position - v0.position;
                Vec3 edge2 = v2.position - v0.position;
                v0.normal = v1.normal = v2.normal = v3.normal = glm::normalize(glm::cross(edge2, edge1));

                mesh.vertices.push_back(v0);
                mesh.vertices.push_back(v1);
                mesh.vertices.push_back(v2);
                mesh.vertices.push_back(v3);

                mesh.indices.push_back(baseIdx + 0);
                mesh.indices.push_back(baseIdx + 1);
                mesh.indices.push_back(baseIdx + 2);
                mesh.indices.push_back(baseIdx + 1);
                mesh.indices.push_back(baseIdx + 3);
                mesh.indices.push_back(baseIdx + 2);
            }
        }
    }

    return mesh;
}

WaterMesh TerrainParser::generateWaterMesh(const TerrainFile& terrain,
                                       uint32_t startX, uint32_t startZ,
                                       uint32_t chunkW, uint32_t chunkH,
                                       float waterLevel, float waveHeight,
                                       bool forceAllTiles,
                                       bool skipIfPositive) {
    (void)forceAllTiles;
    WaterMesh wmesh;
    uint32_t W = terrain.width;
    uint32_t H = terrain.height;
    float tileSize = terrain.scale;

    // Source maps with real water can have positive or negative waterLevel.
    // The previous heuristic (skipIfPositive) was incorrect and broke maps like cidade-C.
    if (!forceAllTiles && skipIfPositive && waterLevel == 0.0f) {
        // Only skip if exactly 0.0f (which usually means disabled in source world)
        return wmesh;
    }


    // Build a continuous grid mesh (shared vertices) to eliminate tile seams.
    // Clamp to map bounds.
    uint32_t effW = std::min(chunkW, W - startX);
    uint32_t effH = std::min(chunkH, H - startZ);
    if (effW == 0 || effH == 0) return wmesh;

    // Grid has (effW+1) x (effH+1) vertices.
    uint32_t vertW = effW + 1;
    uint32_t vertH = effH + 1;
    wmesh.vertices.reserve(vertW * vertH);

    for (uint32_t z = 0; z < vertH; z++) {
        uint32_t worldZ = startZ + z;
        for (uint32_t x = 0; x < vertW; x++) {
            uint32_t worldX = startX + x;
            float sx = static_cast<float>(worldX) * tileSize;
            float sz = static_cast<float>(worldZ) * tileSize;

            // UV tiling: 5x5 repeat pattern (like source data)
            float u = (float)(worldX % 5) / 5.0f;
            float v = (float)(worldZ % 5) / 5.0f;

            WaterVertex vtx;
            vtx.position = Vec3(sx, waterLevel, sz);
            vtx.texCoord = Vec2(u, v);
            wmesh.vertices.push_back(vtx);
        }
    }

    // Indices for triangle strips
    uint32_t rowVerts = vertW;
    wmesh.indices.reserve(effW * effH * 6);
    for (uint32_t z = 0; z < effH; z++) {
        for (uint32_t x = 0; x < effW; x++) {
            uint32_t i0 = z * rowVerts + x;
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + rowVerts;
            uint32_t i3 = i2 + 1;

            wmesh.indices.push_back(i0);
            wmesh.indices.push_back(i1);
            wmesh.indices.push_back(i2);
            wmesh.indices.push_back(i1);
            wmesh.indices.push_back(i3);
            wmesh.indices.push_back(i2);

            wmesh.waterTileCount++;
        }
    }

    return wmesh;
}

float TerrainParser::getTerrainHeightAt(const TerrainFile& terrain, float worldX, float worldZ) {
    float tileSize = terrain.scale > 0.0f ? terrain.scale : 10.0f;
    float localX = worldX - terrain.offsetX;
    float localZ = worldZ - terrain.offsetZ;
    int x = static_cast<int>(localX / tileSize);
    int z = static_cast<int>(localZ / tileSize);
    if (x < 0 || z < 0 || x >= static_cast<int>(terrain.width) || z >= static_cast<int>(terrain.height)) return 0.0f;

    const auto& cube = terrain.cubes[z * terrain.width + x];
    float dx = (localX / tileSize) - x;
    float dz = (localZ / tileSize) - z;
    
    // Bilinear interpolation
    float hSW = cube.height[0];
    float hSE = cube.height[1];
    float hNW = cube.height[2];
    float hNE = cube.height[3];

    float h = (hSW * (1.0f - dx) * (1.0f - dz) +
               hSE * dx * (1.0f - dz) +
               hNW * (1.0f - dx) * dz +
               hNE * dx * dz);
    
    return -h * 1.0f;
}

} // namespace eruption
