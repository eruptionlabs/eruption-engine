// Carga de mapa do Engine: loadMap (o caminho sincrono completo - terreno,
// modelos, sprites, agua, clima, spawn do jogador), a amostragem de altura do
// terreno usada pra decidir onde o jogador nasce, e setPlayerController.
// Saiu do Engine.cpp em 2026-09-04, na quebra do arquivo.
//
// O caminho ASSINCRONO (warp com a cena ja' viva) nao esta' aqui: quem faz o
// swap e drena a fila de upload e' o MapUpload.cpp.

#include "core/Engine.hpp"
#include "core/EngineInternal.hpp"
#include "game/PlayerController.hpp"
#include "core/Logger.hpp"
#include "core/JobSystem.hpp"
#include "utils/Profiler.hpp"
#include "utils/TelemetryExporter.hpp"
#include "utils/PbrTextureLoader.hpp"
#include "utils/ImageUtils.hpp"
#include "formats/MapLoader.hpp"
#include "formats/ModelDataConverter.hpp"
#include "renderer/WeatherTypes.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <vk_mem_alloc.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace eruption {

static float sampleGlbTerrainHeight(const LoadedMap& map, float x, float z, float radius = 1.5f) {
    if (map.models.empty()) return 0.0f;
    bool hasTerrainNodes = false;
    for (const auto& model : map.models) {
        for (const auto& node : model.nodes) {
            if (node.name.size() >= 8 && node.name.substr(0, 8) == "terrain_") {
                hasTerrainNodes = true; break;
            }
        }
        if (hasTerrainNodes) break;
    }
    constexpr float INF = 1e9f; float minY = INF; float sumY = 0.0f; int count = 0; float radiusSq = radius * radius;
    for (const auto& model : map.models) {
        for (const auto& node : model.nodes) {
            // Nós que compartilham geometria não guardam vértices: a amostragem
            // usa a malha do dono com a matriz DESTE nó, que é exatamente o que
            // a cópia por instância produzia antes.
            const auto& geo = (node.geometryShareIndex >= 0 &&
                               node.geometryShareIndex < static_cast<int32_t>(model.nodes.size()))
                                  ? model.nodes[node.geometryShareIndex]
                                  : node;
            if (geo.vertices.empty()) continue;
            if (hasTerrainNodes && (node.name.size() < 8 || node.name.substr(0, 8) != "terrain_")) continue;
            const Mat4& m = node.renderMatrix;
            for (const auto& v : geo.vertices) {
                Vec4 world = m * Vec4(v, 1.0f);
                float dx = world.x - x; float dz = world.z - z;
                if (dx * dx + dz * dz <= radiusSq) {
                    minY = glm::min(minY, world.y); sumY += world.y; ++count;
                }
            }
        }
    }
    if (count == 0) return 0.0f;
    float avgY = sumY / count;
    if (minY < avgY - 10.0f) return avgY;
    return minY;
}

static float sampleGlbOrTerrainHeight(const LoadedMap& map, float x, float z) {
    // GLB maps that carry a companion .ter grid should use it for ground height:
    // it is fast, matches the authored terrain exactly, and is not confused by
    // nearby model geometry.  Fall back to vertex sampling for raw GLB maps
    // without a terrain file.
    if (map.isGlbMap && map.terrain.width > 0 && map.terrain.height > 0) {
        return TerrainParser::getTerrainHeightAt(map.terrain, x, z);
    }
    return sampleGlbTerrainHeight(map, x, z, 1.5f);
}


float Engine::sampleTerrainHeight(float x, float z) const {
    if (!m_currentMap) return 0.0f;
    return sampleGlbOrTerrainHeight(*m_currentMap, x, z);
}

void Engine::setPlayerController(PlayerController* pc) {
    m_playerController = pc;
    if (!m_playerController) return;

    if (!m_currentMap) {
        // Map not ready yet (background warp pending). Park the player below the
        // world so the sprite/camera don't render in the void while loading.
        m_playerController->setPos(kSafePreWarpPlayerPos);
        m_camera.setOrbitTarget(kSafePreWarpPlayerPos);
        m_playerSpawnedOnActiveMap = false;
        ERUPTION_LOG_INFO("[Engine] Player controller set before map load; parked at safe position (%.1f, %.1f, %.1f)",
                          kSafePreWarpPlayerPos.x, kSafePreWarpPlayerPos.y, kSafePreWarpPlayerPos.z);
    } else {
        m_playerSpawnedOnActiveMap = true;
    }
}

bool Engine::loadMap(const std::string& name) {
    m_isInitialBoot = false; // No longer the initial boot
    TelemetryExporter::recordLoadStart(name);
    loadMapClimate(name);
    m_vulkan.waitIdle();

    // GERACAO DO CACHE DE ASSET. Ate 2026-09-05 so' o warp (performMapSwap)
    // avancava a geracao e expulsava textura/malha velha do GlobalAssetCache;
    // a carga fria repetida nunca chamava nada disto, entao currentWarpId
    // ficava parado, a expiracao (Engine.cpp, lastSeenWarpId + maxAge) nunca
    // disparava e cada mapa carregado no mesmo processo deixava VkImage +
    // VmaAllocation + slot bindless presos para sempre - com 4096 slots
    // (MAX_BINDLESS_TEXTURES), o esgotamento devolve slot 0 e tudo vira a
    // textura errada, sem crash. Mesma semantica do warp: o que o mapa novo
    // reusar renova a idade; o que ficar 3 cargas sem uso sai.
    m_assetCache.incrementWarp();
    m_assetCache.cleanupOldTextures(&m_vulkan, &m_bindless, 3);

    // Independent clouds belong to the previous map: drop them so stale world
    // bounds / layers don't leak into the newly loaded map. The weather field
    // respawns for the new map on the next update tick.
    clearIndependentClouds();
    m_weatherFieldType = -1;

    presentLoadingScreen(name, "Loading map data", 0.1f);

    // MapLoader accepts either a plain map name (resolved to assets/external/<name>/<name>.glb)
    // or an explicit file path ending in .glb/.map.
    auto loadedMap = m_mapLoader->loadMap(name); 
    if (!loadedMap) {
        ERUPTION_LOG_ERROR("Failed to load map: %s", name.c_str());
        return false;
    }
    
    m_currentMap = loadedMap; 
    m_currentMapName = name;

    // Configure water mesh from map data
    m_water.clearWaterMesh();
    if (loadedMap) {
        float wl, wh;
        bool fromTerrain = getMapWaterParams(*loadedMap, wl, wh, m_waterMenu.config.forceWater);
        auto wmesh = TerrainParser::generateWaterMesh(loadedMap->terrain, 0, 0,
                                                  loadedMap->terrain.width, loadedMap->terrain.height,
                                                  wl, wh, m_waterMenu.config.forceWater, !fromTerrain);
        m_mapBaseWaterLevel = wl;
        m_water.setWaterMesh(wmesh);
        cacheWaterBounds(wmesh);
        syncLiquidMenuFromMap(loadedMap.get());
        setupLavaLiquid(loadedMap.get());
        ERUPTION_LOG_INFO("Water mesh: %u tiles (baseLevel=%.2f, offset=%.2f, src=%s)",
                         wmesh.waterTileCount, m_mapBaseWaterLevel, m_waterMenu.config.waterLevel, fromTerrain ? "terrain data" : "world data");
    }

    presentLoadingScreen(name, "Building terrain", 0.4f);
    if (!loadedMap->isGlbMap) {
        m_terrainRenderer.buildChunks(loadedMap->terrain, &m_packManager);
    } else {
        m_terrainRenderer.clear();
        ERUPTION_LOG_INFO("Skipping terrain renderer build for GLB map '%s'", name.c_str());
    }

    presentLoadingScreen(name, "Placing models", 0.7f);
    if (loadedMap->isGlbMap) {
        // Modern GLB maps use the companion terrain file only for spawn/height reference.
        // Center the player on the authored terrain bounds, honoring offset + scale.
        m_centerX = loadedMap->terrain.offsetX + (static_cast<float>(loadedMap->terrain.width) * loadedMap->terrain.scale) * 0.5f;
        m_centerZ = loadedMap->terrain.offsetZ + (static_cast<float>(loadedMap->terrain.height) * loadedMap->terrain.scale) * 0.5f;
    } else {
        m_centerX = (float)loadedMap->terrain.width * 5.0f;
        m_centerZ = (float)loadedMap->terrain.height * 5.0f;
    }

    // Fit cloud coverage bounds to the actual map size.
    {
        Vec3 worldMin(0.0f, 0.0f, 0.0f);
        Vec3 worldMax(m_centerX * 2.0f, 2000.0f, m_centerZ * 2.0f);
        m_cloudLayerRenderer.coverageArray().setWorldBounds(worldMin, worldMax);
    }

    std::vector<ModelAsset> genericModels;
    genericModels.reserve(loadedMap->models.size());
    for (const auto& model : loadedMap->models) {
        genericModels.push_back(convertModelToAsset(model));
    }
    
    std::vector<ModelInstanceDesc> genericInstances;
    for (const auto& m : loadedMap->models) {
        ModelInstanceDesc d;
        d.modelPath = m.filePath;
        d.scale = Vec3(1.0f);
        d.rotation = Vec3(0.0f);
        genericInstances.push_back(d);
    }
    
    if (name.find("medieval_village") != std::string::npos) {
        for (auto& mod : loadedMap->models) {
            for (auto& node : mod.nodes) {
                for (auto& v : node.vertices) {
                    v *= 5.0f;
                }
                node.position *= 5.0f;
                node.offsetMatrix[3][0] *= 5.0f;
                node.offsetMatrix[3][1] *= 5.0f;
                node.offsetMatrix[3][2] *= 5.0f;
            }
            mod.box.min *= 5.0f;
            mod.box.max *= 5.0f;
        }
        for (auto& mod : genericModels) {
            for (auto& node : mod.nodes) {
                for (auto& v : node.vertices) {
                    v.position *= 5.0f;
                }
                node.aabbMin *= 5.0f;
                node.aabbMax *= 5.0f;
                node.position *= 5.0f;
                node.renderMatrix[3][0] *= 5.0f;
                node.renderMatrix[3][1] *= 5.0f;
                node.renderMatrix[3][2] *= 5.0f;
            }
            mod.boxMin *= 5.0f;
            mod.boxMax *= 5.0f;
        }
    }
    ERUPTION_LOG_WARN("Engine::loadMap: loading %zu models, %zu instances", genericModels.size(), genericInstances.size());
    // GLB maps are authored in world space; do not add a center offset.
    float modelCenterX = loadedMap->isGlbMap ? 0.0f : m_centerX;
    float modelCenterZ = loadedMap->isGlbMap ? 0.0f : m_centerZ;
    // ------------------------------------------------------------------
    // Texturas EMBUTIDAS no GLB -> compressao de bloco (BC) com mips prontos.
    //
    // Este era o buraco da Fase 5: o .etex ja' cobria texturas de DISCO, mas
    // mapas GLB (cidade-A, parana_field) trazem TODAS as texturas dentro do
    // container - 164 PNGs / 137 Mpx no parana - e elas subiam RGBA8, ×3 com
    // os mapas PBR sintetizados. O bake abaixo comprime na primeira carga e
    // cacheia em disco (<glb>.etexpack, invalidado por tamanho+mtime do GLB,
    // teto de resolucao e conjunto de formatos da GPU).
    //
    // ACESSIBILIDADE: o cache e' artefato DERIVADO. Sem ele, ou numa GPU sem
    // BC, m_embeddedBaked fica vazio e o caminho RGBA8 abaixo roda igual.
    // ------------------------------------------------------------------
    m_embeddedBaked.clear();
    if (m_currentMap) {
        for (auto& model : m_currentMap->models) {
            if (model.embeddedTextures.empty()) continue;
            EmbeddedBakeResult bake = bakeEmbeddedTextures(
                model, static_cast<uint32_t>(m_textureMaxSize), pbrSynthesisEnabled());
            // OVERRIDE do .env tem prioridade sobre a media automatica. A
            // media e' tirada de TODAS as texturas embutidas do GLB (no
            // parana_field sao 188: arvore, folha, rocha, lava), entao ela
            // converge para um cinza quase neutro e o lado sombreado de tudo
            // recebe cinza - a maior causa isolada de "cara de 2003" segundo o
            // autor. Um latossolo roxo quer quique laranja saturado, e isso a
            // media nunca vai dar.
            const Vec3& ovr = m_currentMap ? m_currentMap->env.groundAlbedoOverride
                                           : Vec3(-1.0f);
            if (ovr.r >= 0.0f && ovr.g >= 0.0f && ovr.b >= 0.0f) {
                float lum = glm::max(0.2126f * ovr.r + 0.7152f * ovr.g + 0.0722f * ovr.b, 1e-3f);
                m_mapGroundAlbedo = ovr * (0.16f / lum);
                ERUPTION_LOG_WARN("Engine::loadMap: ground albedo do .env = (%.3f, %.3f, %.3f)",
                                  m_mapGroundAlbedo.r, m_mapGroundAlbedo.g, m_mapGroundAlbedo.b);
            } else if (bake.hasGroundAlbedo) {
                Vec3 avg(bake.groundAlbedo[0], bake.groundAlbedo[1], bake.groundAlbedo[2]);
                float lum = glm::max(0.2126f * avg.r + 0.7152f * avg.g + 0.0722f * avg.b, 1e-3f);
                m_mapGroundAlbedo = avg * (0.16f / lum);
                ERUPTION_LOG_WARN("Engine::loadMap: map ground albedo = (%.3f, %.3f, %.3f)",
                                  m_mapGroundAlbedo.r, m_mapGroundAlbedo.g, m_mapGroundAlbedo.b);
            }
            for (auto& [name, tex] : bake.textures) {
                m_embeddedBaked.emplace(name, std::move(tex));
            }
        }
    }

    // FAST PARALLEL DECODE of all embedded GLB textures
    // (so' as que o bake NAO cobriu - fallback puro RGBA8)
    std::vector<EmbeddedTexture*> allEmbedded;
    if (m_currentMap) {
        for (auto& model : m_currentMap->models) {
            for (auto& et : model.embeddedTextures) {
                if (m_embeddedBaked.count(et.name)) continue;
                if (!et.isDecoded() && et.hasEncodedData()) allEmbedded.push_back(&et);
            }
        }
    }
    const int EMBEDDED_TEXTURE_MAX_SIZE = m_textureMaxSize;
    if (!allEmbedded.empty()) {
        ERUPTION_LOG_WARN("Engine::loadMap: Parallel decoding %zu embedded textures...", allEmbedded.size());
        size_t downscaled = 0;
        std::mutex statMutex;
        eruption::JobSystem::instance().parallelFor(allEmbedded.size(), [&](uint32_t i) {
            EmbeddedTexture* et = allEmbedded[i];
            ImageData img = ImageUtils::loadFromMemoryRaw(et->encodedData.data(), et->encodedData.size());
            if (img.isValid()) {
                if (ImageUtils::downscaleRGBA(img.width, img.height, img.pixels, EMBEDDED_TEXTURE_MAX_SIZE)) {
                    std::lock_guard<std::mutex> lock(statMutex);
                    ++downscaled;
                }
                et->pixels = std::move(img.pixels);
                et->width = img.width;
                et->height = img.height;
                et->channels = img.channels;
            }
        });
        ERUPTION_LOG_WARN("Engine::loadMap: Embedded textures downscaled: %zu / %zu", downscaled, allEmbedded.size());

        // Average albedo of the map's textures -> real per-map bounce/ambient
        // ground tint (vila-A bounces green-ish grass light, parana red laterite).
        {
            double r = 0, g = 0, b = 0, n = 0;
            for (const EmbeddedTexture* et : allEmbedded) {
                if (!et->isDecoded() || et->channels < 3) continue;
                const int stride = et->channels;
                const size_t px = static_cast<size_t>(et->width) * et->height;
                for (size_t i = 0; i < px; i += 97) { // sparse sample
                    const uint8_t* p = &et->pixels[i * stride];
                    // Skip magenta colour-key and near-black borders.
                    if (p[0] > 230 && p[1] < 25 && p[2] > 230) continue;
                    if (p[0] + p[1] + p[2] < 24) continue;
                    r += p[0]; g += p[1]; b += p[2]; n += 1;
                }
            }
            if (n > 100) {
                Vec3 avg(static_cast<float>(r / n) / 255.0f,
                         static_cast<float>(g / n) / 255.0f,
                         static_cast<float>(b / n) / 255.0f);
                // Normalise to a bounce-appropriate luminance so a bright map
                // doesn't blow out the indirect term and a dark one doesn't kill it.
                float lum = glm::max(0.2126f * avg.r + 0.7152f * avg.g + 0.0722f * avg.b, 1e-3f);
                m_mapGroundAlbedo = avg * (0.16f / lum);
                ERUPTION_LOG_WARN("Engine::loadMap: map ground albedo = (%.3f, %.3f, %.3f)",
                                  m_mapGroundAlbedo.r, m_mapGroundAlbedo.g, m_mapGroundAlbedo.b);
            } else {
                m_mapGroundAlbedo = Vec3(0.08f, 0.06f, 0.04f);
            }
        }
    }

    // Decode all cooked PBR maps (_mrahw/_normal PNGs) in parallel before the
    // serial mesh loop. This was the bulk of map-load time.
    {
        auto _t = std::chrono::steady_clock::now();
        std::vector<std::string> pbrNames;
        for (const auto& m : genericModels) {
            for (const auto& node : m.nodes) {
                for (const auto& n : node.pbrTextureNames) if (!n.empty()) pbrNames.push_back(n);
                for (const auto& n : node.textureNames)    if (!n.empty()) pbrNames.push_back(n);
            }
        }
        eruption::prewarmPbrTextureData(pbrNames);
        ERUPTION_LOG_WARN("Engine::loadMap: PBR prewarm (%zu names) took %lld ms", pbrNames.size(),
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _t).count());
    }

    auto _tLMM = std::chrono::steady_clock::now();
    m_modelRenderer.loadMapModels(genericModels, genericInstances, modelCenterX, modelCenterZ);
    eruption::clearPbrTextureDataPrewarm(); // free any pre-decoded PBR data not consumed
    ERUPTION_LOG_WARN("Engine::loadMap: loadMapModels took %lld ms",
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _tLMM).count());

    // FLUSH remaining embedded texture uploads
    if (!s_pendingUploads.empty()) {
        m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
            for (auto& u : s_pendingUploads) {
                m_vulkan.cmdImageBarrier(cmd, u.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                if (!u.mipSizes.empty()) {
                    // Textura comprimida em bloco: mips vem prontos do bake.
                    std::vector<VkBufferImageCopy> regions(u.mipSizes.size());
                    VkDeviceSize off = 0;
                    for (uint32_t m = 0; m < u.mipSizes.size(); ++m) {
                        VkBufferImageCopy& r = regions[m];
                        r = {};
                        r.bufferOffset = off;
                        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        r.imageSubresource.mipLevel = m;
                        r.imageSubresource.layerCount = 1;
                        r.imageExtent = { std::max(1u, u.width >> m), std::max(1u, u.height >> m), 1 };
                        off += u.mipSizes[m];
                    }
                    vkCmdCopyBufferToImage(cmd, u.staging, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           static_cast<uint32_t>(regions.size()), regions.data());
                    m_vulkan.cmdImageBarrier(cmd, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
                    continue;
                }
                VkBufferImageCopy region{};
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = { u.width, u.height, 1 };
                vkCmdCopyBufferToImage(cmd, u.staging, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                if (u.mipLevels > 1) {
                    this->generateMipmaps(cmd, u.image, u.width, u.height, u.mipLevels, VK_FORMAT_R8G8B8A8_UNORM, 1);
                } else {
                    m_vulkan.cmdImageBarrier(cmd, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
                }
            }
        });
        for (auto& u : s_pendingUploads) {
            vmaDestroyBuffer(m_vulkan.allocator(), u.staging, u.stagingAlloc);
        }
        s_pendingUploads.clear();
    }

    flushPbrTextureUploads(&m_vulkan);
    
    if (!s_pendingMeshes.empty()) {
        m_vulkan.immediateSubmit([&](VkCommandBuffer cmd) {
            for (auto& pm : s_pendingMeshes) {
                VkBufferCopy cv{0, 0, pm.vSize};
                vkCmdCopyBuffer(cmd, pm.staging, pm.vb, 1, &cv);
                VkBufferCopy ci{pm.vSize, 0, pm.iSize};
                vkCmdCopyBuffer(cmd, pm.staging, pm.ib, 1, &ci);
            }
        });
        for (auto& pm : s_pendingMeshes) {
            vmaDestroyBuffer(m_vulkan.allocator(), pm.staging, pm.alloc);
        }
        s_pendingMeshes.clear();
    }


    ERUPTION_LOG_WARN("Engine::loadMap: ModelRenderer has %zu meshes, %zu instances, "
                      "%llu triangulos carregados (malha base x instancia, pre-cull/LOD)",
                      m_modelRenderer.meshCount(), m_modelRenderer.instanceCount(),
                      static_cast<unsigned long long>(m_modelRenderer.loadedTriangles()));
    
    presentLoadingScreen(name, "Finalizing", 0.95f);
    if (loadedMap) {
        // OFFSET DO CENTRO SO' EM MAPA LEGADO. As luzes de um mapa legado vem em
        // coordenada RELATIVA ao centro do mapa; as de um GLB (o .env ao lado)
        // vem em coordenada de MUNDO ja' pronta. Somar o centro nas duas fazia
        // as luzes do GLB irem parar FORA do mapa: no parana_field, as 12 luzes
        // de lava nasciam em (2807,2739) e eram jogadas para (4807,4739), num
        // mapa de 4000x4000 - nenhuma delas iluminava nada, e o que restava era
        // a luz presa ao jogador (autor 2026-09-06: "a luz dinamica ta muito
        // fraca"). Medido: intensidade de 0 a 27 nao mudava UM pixel.
        const Vec3 lightOrigin = loadedMap->isGlbMap ? Vec3(0.0f)
                                                 : Vec3(m_centerX, 0.0f, m_centerZ);
        std::vector<PointLight> lights;
        lights.reserve(loadedMap->lights.size());
        for (const auto& ml : loadedMap->lights) {
            PointLight pl;
            pl.position = ml.position + lightOrigin;
            pl.color = ml.color;
            pl.intensity = ml.intensity;
            pl.radius = ml.range;
            pl.animType = ml.animType;
            pl.nightOnly = ml.nightOnly;
            pl.enabled = !ml.nightOnly;
            if (std::getenv("ERUPTION_DEBUG_LAVA_LIGHT"))
                ERUPTION_LOG_WARN("[PLIGHT] fonte (%.0f,%.0f,%.0f) -> mundo (%.0f,%.0f,%.0f) centro (%.0f,%.0f) r=%.0f i=%.2f",
                                  ml.position.x, ml.position.y, ml.position.z,
                                  pl.position.x, pl.position.y, pl.position.z,
                                  m_centerX, m_centerZ, pl.radius, pl.intensity);
            lights.push_back(pl);
        }
        appendArtificialTestLights(m_currentMapName, m_centerX, m_centerZ, lights);
        m_deferredLighting.setPointLights(lights);
    } else {
        m_deferredLighting.setPointLights({});
    }
    Vec3 spawnPos = Vec3(m_centerX, 0.0f, m_centerZ);
    if (loadedMap) {
        if (loadedMap->isGlbMap) {
            // GLB maps are authored in world space. Spawn at the map center and
            // Snap spawn Y to the companion terrain grid when available, otherwise
            // fall back to sampling the authored terrain geometry.
            // GLB maps have precise authored ground; keep the character exactly on
            // the surface instead of floating 0.5 units above it.
            spawnPos.y = clampSpawnYToWater(*loadedMap,
                                            sampleGlbOrTerrainHeight(*loadedMap, spawnPos.x, spawnPos.z));
        } else {
            spawnPos.y = clampSpawnYToWater(*loadedMap,
                                            TerrainParser::getTerrainHeightAt(loadedMap->terrain, spawnPos.x, spawnPos.z));
        }
    }
    // Default orbit target sits slightly above the ground so the camera looks
    // toward the horizon rather than through the floor.
    m_camera.setOrbitTarget(spawnPos + Vec3(0.0f, 7.2f, 0.0f));
    m_playerController->setPos(spawnPos);
    LightingEnvironment env;
    env.sun.direction = loadedMap->env.sunDirection;
    if (glm::length(env.sun.direction) < 0.001f) env.sun.direction = glm::normalize(Vec3(-0.5f, -1.0f, -0.3f));
    env.sun.color = loadedMap->env.sunColor;
    env.sun.intensity = loadedMap->env.sunIntensity;
    env.ambientColor = loadedMap->env.ambientColor;
    env.ambientIntensity = loadedMap->env.ambientIntensity;
    m_deferredLighting.setEnvironment(env);

    if (m_hasInitialPos) {
        Vec3 initialPos = m_initialPos;
        if (loadedMap) {
            if (loadedMap->isGlbMap) {
                // GLB maps keep authored world-space X/Z; snap Y to the companion
                // terrain grid when available, otherwise sample authored geometry.
                // Keep the character on the authored GLB ground surface.
                initialPos.y = clampSpawnYToWater(*loadedMap,
                                                  sampleGlbOrTerrainHeight(*loadedMap, initialPos.x, initialPos.z));
            } else {
                initialPos.y = clampSpawnYToWater(*loadedMap,
                                                  TerrainParser::getTerrainHeightAt(loadedMap->terrain, initialPos.x, initialPos.z));
            }
        }
        m_playerController->setPos(initialPos);
        m_camera.setOrbitTarget(m_playerController->pos());
    }

    if (m_hasInitialCamera) {
        m_camera.setOrbit(m_initialYaw, m_initialPitch, m_initialDist);
        m_camera.setDefaultOrbit(m_initialYaw, m_initialPitch, m_initialDist);
    } else {
        // Default spawn orientation: look toward the south, 50° pitch, 85% zoom.
        // Zoom 0.85 maps to orbit distance = 10 + (1 - 0.85) * (1000 - 10) = 158.5.
        float defaultDist = 10.0f + (1.0f - 0.85f) * (1000.0f - 10.0f);
        m_camera.setOrbit(glm::radians(-90.0f), glm::radians(50.0f), defaultDist);
        m_camera.setDefaultOrbit(glm::radians(-90.0f), glm::radians(50.0f), defaultDist);
    }
    m_playerController->setYaw(glm::pi<float>());
    m_playerSpawnedOnActiveMap = true;

    m_vulkan.waitIdle();

    ERUPTION_LOG_WARN("Player spawned at (%.2f, %.2f, %.2f)",
                    m_playerController->pos().x,
                    m_playerController->pos().y,
                    m_playerController->pos().z);

    // Optional debug auto-spawn (off by default; enable with --auto-cloud).
    {
        CloudLayerRenderer::Config cfg = m_cloudLayerRenderer.config();
        cfg.enabled = true;
        m_cloudLayersEnabled = true;
        m_cloudLayerRenderer.setConfig(cfg);
        m_cloudLayerRenderer.coverageArray().setLayerCount(cfg.layerCount);
    }

    // PROBES DE IRRADIANCIA (G38): bake 2.5D da visibilidade do ceu sobre o
    // (NO FIM do loadMap, depois de TODOS os uploads e do waitIdle: os buffers
    // base das malhas ainda nao estavam na GPU no ponto anterior - so' as
    // malhas com LOD, enviadas na hora, rasterizavam; chao e modelos legados
    // saiam vazios dos heightfields.)
    // AABB do terreno (+ margem vertical para copas/telhados). Terreno e
    // modelos ja' estao na GPU; o pipeline de sombra desenha os tres mapas de
    // profundidade. Sem terreno (mapa vazio) fica o fallback "ceu aberto".
    {
        Vec3 mn(1e30f), mx(-1e30f);
        for (const auto& ch : m_terrainRenderer.chunks()) {
            mn = glm::min(mn, ch.aabbMin);
            mx = glm::max(mx, ch.aabbMax);
        }
        // Mapa GLB: o terreno e' modelo - o AABB vem das instancias.
        Vec3 imn, imx;
        if (m_modelRenderer.sceneBounds(imn, imx)) {
            mn = glm::min(mn, imn);
            mx = glm::max(mx, imx);
        }
        const bool haveBounds = mx.x > mn.x && mx.z > mn.z;
        if (haveBounds && m_irradianceProbesEnabled) {
            mx.y += 40.0f;
            mn.y -= 20.0f;
            if (m_irradianceProbes.bake(m_terrainRenderer, m_modelRenderer,
                                        m_shadowRenderer.shadowPipeline(), m_shadowRenderer.shadowLayout(),
                                        m_shadowRenderer.shadowInstPipeline(), m_shadowRenderer.shadowInstLayout(),
                                        &m_bindless, mn, mx)) {
                m_deferredLighting.setIrradianceProbes(m_irradianceProbes.view(), m_irradianceProbes.sampler(),
                                                       m_irradianceProbes.gridMin(), m_irradianceProbes.gridInvExtent(), true);
            }
        } else {
            m_irradianceProbes.clear();
            m_deferredLighting.setIrradianceProbes(m_irradianceProbes.view(), m_irradianceProbes.sampler(),
                                                   Vec3(0.0f), Vec3(0.0f), false);
        }
    }

    TelemetryExporter::recordLoadEnd(name);
    // Estado do cache cross-mapa depois desta carga. E' o detector do
    // vazamento que existia ate 2026-09-05 (carga fria nunca expulsava nada):
    // em loop de troca de mapa, model/terrain/meshes e usedSlots tem que
    // estabilizar a partir da 4a carga; highWater e' a marca d'agua dos 4096.
    {
        const auto cs = m_assetCache.getStats();
        ERUPTION_LOG_WARN("[ASSETCACHE] gen=%u modelTex=%zu terrainTex=%zu meshes=%zu bindless_used=%u highWater=%u/%u",
                          m_assetCache.currentWarpId, cs.modelTextureCount, cs.terrainTextureCount, cs.meshCount,
                          m_bindless.usedSlots(), m_bindless.highWater(), MAX_BINDLESS_TEXTURES);
    }
    return true;
}

} // namespace eruption
