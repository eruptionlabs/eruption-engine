#include "core/BackgroundMapLoader.hpp"
#include "core/JobSystem.hpp"
#include "core/Logger.hpp"
#include "utils/ImageUtils.hpp"
#include "utils/Profiler.hpp"
#include "utils/PbrTextureLoader.hpp"
#include "utils/EmbeddedTextureBake.hpp"
#include "formats/ModelDataConverter.hpp"


#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static std::vector<uint8_t> readFileBytes(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(f.tellg());
    if (size == 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

namespace eruption {

static std::string normalizeTexturePath(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.front() == '/') p = p.substr(1);
    // Strip the source pack container prefix so "data/texture/model/.../x.bmp"
    // resolves to "assets/data/texture/model/.../x.bmp" instead of a doubled path.
    if (p.rfind("data/texture/", 0) == 0) {
        p = p.substr(13);
    } else if (p.rfind("data/", 0) == 0) {
        p = p.substr(5);
    } else if (p.rfind("texture/", 0) == 0) {
        p = p.substr(8);
    }
    return p;
}

BackgroundMapLoader::BackgroundMapLoader(AssetCacheInterface cache)
    : m_mapLoader(std::make_unique<MapLoader>())
    , m_cache(cache)
{
}

BackgroundMapLoader::~BackgroundMapLoader() {
    m_cancelled.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool BackgroundMapLoader::startLoad(const std::string& mapName) {
    if (m_loading.load()) return false;

    // Ensure any previous background thread is joined before starting a new one.
    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_loading.store(true);
    m_cancelled.store(false);
    m_progress.store(0.0f);
    m_currentMapName = mapName;

    m_promise = std::promise<std::unique_ptr<MapContext>>();
    m_future = m_promise.get_future();

    m_thread = std::thread([this, mapName]() {
        auto result = loadJob(mapName);
        try {
            m_promise.set_value(std::move(result));
        } catch (...) {
            m_promise.set_exception(std::current_exception());
        }
    });

    return true;
}

void BackgroundMapLoader::cancel() {
    m_cancelled.store(true);
}

std::unique_ptr<MapContext> BackgroundMapLoader::pollResult() {
    if (!m_future.valid()) return nullptr;

    if (m_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_loading.store(false);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        try {
            return m_future.get();
        } catch (const std::exception& e) {
            ERUPTION_LOG_ERROR("BackgroundMapLoader: exception during load: %s", e.what());
            return nullptr;
        }
    }
    return nullptr;
}

std::unique_ptr<MapContext> BackgroundMapLoader::waitResult() {
    if (!m_future.valid()) return nullptr;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_loading.store(false);
    try {
        return m_future.get();
    } catch (const std::exception& e) {
        ERUPTION_LOG_ERROR("BackgroundMapLoader: exception during load: %s", e.what());
        return nullptr;
    }
}

std::unique_ptr<MapContext> BackgroundMapLoader::loadJob(const std::string& mapName) {
    using Clock = std::chrono::high_resolution_clock;
    [[maybe_unused]] auto t0 = Clock::now();
    [[maybe_unused]] auto elapsedMs = [&](const Clock::time_point& t) {
        return std::chrono::duration<float, std::milli>(Clock::now() - t).count();
    };

    auto ctx = std::make_unique<MapContext>();
    ctx->mapName = mapName;

    // --- Phase 1: Parse map files ---
    m_progress.store(0.05f);
    [[maybe_unused]] auto tPhase1 = Clock::now();
    // MapLoader now expects a plain map name or explicit GLB path.
    auto loadedMap = m_mapLoader->loadMap(mapName);
    if (!loadedMap || m_cancelled.load()) {
        ERUPTION_LOG_ERROR("BackgroundMapLoader: failed to load map %s", mapName.c_str());
        m_progress.store(1.0f);
        ctx->cpuReady.store(true);
        return ctx;
    }

    ctx->loadedMap = loadedMap;
    ctx->centerX = static_cast<float>(loadedMap->terrain.width) * 5.0f;
    ctx->centerZ = static_cast<float>(loadedMap->terrain.height) * 5.0f;
    ERUPTION_LOG_INFO("BackgroundMapLoader: phase 1 (parse) took %.1f ms for '%s'",
                      elapsedMs(tPhase1), mapName.c_str());
    m_progress.store(0.3f);

    // --- Phase 2: Prepare terrain CPU data ---
    [[maybe_unused]] auto tPhase2 = Clock::now();
    prepareTerrainData(*ctx);
    if (m_cancelled.load()) {
        ctx->cpuReady.store(true);
        return ctx;
    }
    ERUPTION_LOG_INFO("BackgroundMapLoader: phase 2 (terrain) took %.1f ms for '%s'",
                      elapsedMs(tPhase2), mapName.c_str());
    m_progress.store(0.55f);

    // --- Phase 3: Prepare model CPU data ---
    [[maybe_unused]] auto tPhase3 = Clock::now();
    prepareModelData(*ctx);
    if (m_cancelled.load()) {
        ctx->cpuReady.store(true);
        return ctx;
    }
    ERUPTION_LOG_INFO("BackgroundMapLoader: phase 3 (models) took %.1f ms for '%s'",
                      elapsedMs(tPhase3), mapName.c_str());
    m_progress.store(0.95f);

    ctx->cpuReady.store(true);
    m_progress.store(1.0f);
    ERUPTION_LOG_INFO("BackgroundMapLoader: CPU load complete for '%s' (total %.1f ms)",
                      mapName.c_str(), elapsedMs(t0));
    return ctx;
}

std::vector<uint8_t> BackgroundMapLoader::loadTextureData(const std::string& texPath,
                                                          std::string* outResolvedPath) {
    std::string normalized = normalizeTexturePath(texPath);
    if (normalized.empty()) return {};

    std::string filename = normalized;
    size_t lastSlash = normalized.find_last_of('/');
    if (lastSlash != std::string::npos) {
        filename = normalized.substr(lastSlash + 1);
    }

    // Prefer cooked/modern assets on disk, then fall back to the pack archive.
    const std::vector<std::string> fsAttempts = {
        "assets/data/texture/" + normalized,
        "assets/data/texture/" + filename,
        "assets/data/" + normalized,
        "assets/data/" + filename,
        "assets/pbr/base/" + normalized,
        "assets/pbr/base/" + filename,
    };
    for (const auto& p : fsAttempts) {
        if (fs::exists(p)) {
            auto data = readFileBytes(p);
            if (!data.empty()) {
                ERUPTION_LOG_INFO("BackgroundMapLoader: loaded texture from filesystem: %s", p.c_str());
                if (outResolvedPath) *outResolvedPath = p;
                return data;
            }
        }
    }

    // Fallback: search immediate subdirectories of assets/data/texture and
    // assets/pbr/base for the filename. This matches Engine::resolveModelTexture
    // and covers maps whose albedo directory prefix differs from the cooked
    // asset layout.
    const std::vector<std::string> searchRoots = {
        "assets/data/texture",
        "assets/pbr/base",
    };
    for (const std::string& root : searchRoots) {
        if (fs::exists(root) && fs::is_directory(root)) {
            for (const auto& entry : fs::directory_iterator(root)) {
                if (!entry.is_directory()) continue;
                std::string p = (entry.path() / filename).string();
                if (fs::exists(p)) {
                    auto data = readFileBytes(p);
                    if (!data.empty()) {
                        ERUPTION_LOG_INFO("BackgroundMapLoader: loaded texture from filesystem subdir: %s", p.c_str());
                        if (outResolvedPath) *outResolvedPath = p;
                        return data;
                    }
                }
            }
        }
    }

    return {};
}

void BackgroundMapLoader::preloadPbrForAlbedo(const std::string& albedoPath,
                                              const std::string& pbrName,
                                              PbrTextureData& outMrahw,
                                              PbrTextureData& outNormal) {
    if (albedoPath.empty()) return;

    if (loadIndustryStandardPbr(albedoPath, outMrahw, outNormal)) return;
    if (!pbrName.empty() && loadIndustryStandardPbr(pbrName, outMrahw, outNormal)) return;

    PbrTexturePathPair paths = resolvePbrTexturePaths(albedoPath);
    if (!paths.valid && !pbrName.empty()) {
        paths = resolvePbrTexturePaths(pbrName);
    }
    if (!paths.valid) {
        // No cooked maps: synthesize here on the worker thread. Without this,
        // the render thread's upload fallback (loadPbrTexturesForAlbedo) does
        // the disk scan + Sobel/blur synthesis inside the frame budget.
        if (!synthesizePbrForAlbedoKey(albedoPath, outMrahw, outNormal) &&
            !pbrName.empty()) {
            synthesizePbrForAlbedoKey(pbrName, outMrahw, outNormal);
        }
        return;
    }

    outMrahw = loadPbrTextureData(paths.mrahw);
    outNormal = loadPbrTextureData(paths.normal);

}

void BackgroundMapLoader::prepareTerrainData(MapContext& ctx) {
    if (ctx.loadedMap->isGlbMap) {
        ERUPTION_LOG_INFO("BackgroundMapLoader: skipping terrain data for GLB map '%s'", ctx.mapName.c_str());
        return;
    }
    const auto& terrain = ctx.loadedMap->terrain;
    if (terrain.width == 0 || terrain.height == 0) return;

    std::vector<MapContext::CpuTerrainTex> tempTerrainTex(terrain.textures.size());
    std::atomic<size_t> terrainRamUsage{0};

    eruption::JobSystem::instance().parallelFor(terrain.textures.size(), [&](uint32_t i) {
        if (m_cancelled.load()) return;
        const std::string& texPath = terrain.textures[i];
        if (texPath.empty()) return;

        if (m_cache.hasTerrainTexture && m_cache.hasTerrainTexture(texPath)) {
            MapContext::CpuTerrainTex cpuTex;
            cpuTex.path = texPath;
            cpuTex.pixels = {}; // already on GPU
            cpuTex.terrainIndex = static_cast<uint16_t>(i);
            // PBR maps are per-MapContext even when the albedo is cached; preload
            // them now on the background thread so the main thread does not stall
            // on synchronous I/O during warp.
            preloadPbrForAlbedo(texPath, {}, cpuTex.pbrMrahw, cpuTex.pbrNormal);
            tempTerrainTex[i] = std::move(cpuTex);
            return;
        }

        std::string resolvedPath;
        std::vector<uint8_t> data = loadTextureData(texPath, &resolvedPath);
        if (data.empty()) {
            ERUPTION_LOG_WARN("BackgroundMapLoader: missing terrain texture: %s", texPath.c_str());
            return;
        }

        MapContext::CpuTerrainTex cpuTex;
        cpuTex.path = texPath;
        cpuTex.terrainIndex = static_cast<uint16_t>(i);
        // Baked BC texture: skip decode + dilate entirely.
        cpuTex.etex = loadEtexForImage(resolvedPath);
        if (cpuTex.etex.valid()) {
            cpuTex.width = static_cast<int>(cpuTex.etex.width);
            cpuTex.height = static_cast<int>(cpuTex.etex.height);
            cpuTex.channels = 4;
        } else {
            ImageData img = ImageUtils::loadFromMemory(data.data(), data.size());
            if (!img.isValid()) {
                ERUPTION_LOG_WARN("BackgroundMapLoader: failed to decode terrain texture: %s", texPath.c_str());
                return;
            }
            cpuTex.pixels = std::move(img.pixels);
            cpuTex.width = img.width;
            cpuTex.height = img.height;
            cpuTex.channels = img.channels;
        }

        // Pre-decode PBR maps on the background thread so the main thread only
        // has to upload them to the GPU.
        preloadPbrForAlbedo(texPath, {}, cpuTex.pbrMrahw, cpuTex.pbrNormal);

        terrainRamUsage.fetch_add(cpuTex.pixels.size() + cpuTex.pbrMrahw.pixels.size()
                                  + cpuTex.pbrNormal.pixels.size(), std::memory_order_relaxed);
        tempTerrainTex[i] = std::move(cpuTex);
    });

    if (m_cancelled.load()) return;

    for (auto& tex : tempTerrainTex) {
        if (!tex.path.empty()) {
            ctx.cpuTerrainTextures.push_back(std::move(tex));
        }
    }
    PROFILE_RAM_ALLOC(ProfilerCategory::Textures, terrainRamUsage.load());

    const uint32_t chunkSize = 32;
    uint32_t numChunksX = (terrain.width + chunkSize - 1) / chunkSize;
    uint32_t numChunksZ = (terrain.height + chunkSize - 1) / chunkSize;

    // Generate all chunks but sort by distance from map center so nearby chunks upload first
    struct ChunkWithDist {
        TerrainMesh mesh;
        float distSq;
    };
    float centerCX = static_cast<float>(numChunksX) * 0.5f;
    float centerCZ = static_cast<float>(numChunksZ) * 0.5f;

    // Chunk meshes are independent: generate them on the job system instead
    // of a single-thread double loop (this was one of the serial long poles
    // of the load).
    const uint32_t totalChunks = numChunksX * numChunksZ;
    std::vector<ChunkWithDist> chunkSlots(totalChunks);
    std::vector<uint8_t> chunkValid(totalChunks, 0);
    eruption::JobSystem::instance().parallelFor(totalChunks, [&](uint32_t idx) {
        if (m_cancelled.load()) return;
        const uint32_t cx = idx % numChunksX;
        const uint32_t cz = idx / numChunksX;
        uint32_t startX = cx * chunkSize;
        uint32_t startZ = cz * chunkSize;
        uint32_t cw = std::min(chunkSize, terrain.width - startX);
        uint32_t ch = std::min(chunkSize, terrain.height - startZ);

        TerrainMesh mesh = TerrainParser::generateMesh(terrain, startX, startZ, cw, ch);
        if (!mesh.vertices.empty()) {
            float dx = static_cast<float>(cx) - centerCX;
            float dz = static_cast<float>(cz) - centerCZ;
            chunkSlots[idx] = {std::move(mesh), dx * dx + dz * dz};
            chunkValid[idx] = 1;
        }
    });

    std::vector<ChunkWithDist> chunks;
    chunks.reserve(totalChunks);
    for (uint32_t i = 0; i < totalChunks; ++i) {
        if (chunkValid[i]) chunks.push_back(std::move(chunkSlots[i]));
    }

    std::sort(chunks.begin(), chunks.end(), [](const ChunkWithDist& a, const ChunkWithDist& b) {
        return a.distSq < b.distSq;
    });

    for (auto& chunk : chunks) {
        size_t meshBytes = chunk.mesh.vertices.size() * sizeof(TerrainVertex) + chunk.mesh.indices.size() * sizeof(uint32_t);
        ctx.cpuTerrainMeshes.push_back(std::move(chunk.mesh));
        PROFILE_RAM_ALLOC(ProfilerCategory::Meshes, meshBytes);
    }
}

void BackgroundMapLoader::prepareModelData(MapContext& ctx) {
    const auto& models = ctx.loadedMap->models;
    if (models.empty()) return;

    // 1. Collect all unique textures across all loaded models.
    //    Also remember the original material name (pbrTextureName) for each
    //    runtime albedo name, because embedded GLB names like
    //    "#_____________abc_______01_0000.png" do not match cooked PBR maps.
    std::vector<std::string> allUniqueTextures;
    std::unordered_map<std::string, uint32_t> texPathToGlobalIdx;
    std::unordered_map<std::string, std::string> texPathToPbrName;

    auto sanitizeTex = [&](const std::string& rawPath) {
        std::string p = rawPath;
        std::replace(p.begin(), p.end(), '/', '\\');
        if (!p.empty() && p[0] == '\\') p = p.substr(1);
        return p;
    };

    auto addTexture = [&](const std::string& rawPath) {
        std::string p = sanitizeTex(rawPath);
        if (texPathToGlobalIdx.find(p) == texPathToGlobalIdx.end()) {
            texPathToGlobalIdx[p] = static_cast<uint32_t>(allUniqueTextures.size());
            allUniqueTextures.push_back(p);
        }
    };

    std::vector<const ModelFile*> usedRsms;
    std::unordered_map<std::string, bool> rsmProcessed;
    for (const auto& model : models) {
        if (model.nodes.empty()) continue;
        if (!rsmProcessed[model.filePath]) {
            usedRsms.push_back(&model);
            for (const auto& t : model.textures) addTexture(t);
            for (const auto& node : model.nodes) {
                for (size_t i = 0; i < node.textureNames.size(); ++i) {
                    const std::string& t = node.textureNames[i];
                    std::string p = sanitizeTex(t);
                    addTexture(t);
                    if (i < node.pbrTextureNames.size() && !node.pbrTextureNames[i].empty()) {
                        // Keep the first non-empty PBR name we see for this albedo.
                        if (texPathToPbrName.find(p) == texPathToPbrName.end()) {
                            texPathToPbrName[p] = node.pbrTextureNames[i];
                        }
                    }
                }
            }
            rsmProcessed[model.filePath] = true;
        }
    }

    // Convert used source models to generic ModelAssets for the renderer
    std::unordered_map<const ModelFile*, uint32_t> modelToAssetIdx;
    for (const ModelFile* modelPtr : usedRsms) {
        uint32_t idx = static_cast<uint32_t>(ctx.modelAssets.size());
        ctx.modelAssets.push_back(convertModelToAsset(*modelPtr));
        modelToAssetIdx[modelPtr] = idx;
    }

    // Build a lookup of embedded textures provided by the loaded models (e.g. GLB images).
    std::unordered_map<std::string, const EmbeddedTexture*> embeddedTextureMap;
    for (const ModelFile* modelPtr : usedRsms) {
        for (const auto& et : modelPtr->embeddedTextures) {
            embeddedTextureMap[et.name] = &et;
        }
    }

    // Mesmo bake de bloco (BC) que o caminho de boot usa. No warp isso quase
    // sempre e' um cache HIT em disco (<glb>.etexpack): sem decode de PNG, sem
    // sintese de PBR e sem RGBA8 residente durante a troca de mapa - que e'
    // exatamente onde o orcamento de upload por frame aperta.
    std::unordered_map<std::string, BakedEmbeddedTexture> bakedEmbedded;
    for (const ModelFile* modelPtr : usedRsms) {
        if (modelPtr->embeddedTextures.empty() || modelPtr->filePath.empty()) continue;
        EmbeddedBakeResult bake = bakeEmbeddedTextures(
            *modelPtr, embeddedTextureMaxSize(), pbrSynthesisEnabled());
        for (auto& [name, tex] : bake.textures) {
            bakedEmbedded.emplace(name, std::move(tex));
        }
    }

    // 2. Decode all textures in parallel
    std::vector<MapContext::CpuModelTex> decodedTextures(allUniqueTextures.size());
    std::vector<bool> texCached(allUniqueTextures.size(), false);
    std::atomic<size_t> modelRamUsage{0};

    eruption::JobSystem::instance().parallelFor(allUniqueTextures.size(), [&](uint32_t t) {
        if (m_cancelled.load()) return;
        const std::string& texPath = allUniqueTextures[t];

        if (m_cache.hasModelTexture && m_cache.hasModelTexture(texPath)) {
            texCached[t] = true;
            // The albedo lives in the global cache, but PBR maps are owned per
            // MapContext. Preload them here so the main thread does not block on
            // disk I/O during the warp/upload phase.
            MapContext::CpuModelTex cpuTex;
            cpuTex.path = texPath;
            auto pbrIt = texPathToPbrName.find(texPath);
            const std::string& pbrLookup = (pbrIt != texPathToPbrName.end()) ? pbrIt->second : texPath;
            preloadPbrForAlbedo(texPath, pbrLookup, cpuTex.pbrMrahw, cpuTex.pbrNormal);
            decodedTextures[t] = std::move(cpuTex);
            return;
        }

        // Prefer embedded textures over archive lookups.
        // Same clamp as the synchronous boot path (Engine::loadMap): a warp
        // must not upload 4096^2 embedded textures the boot path would have
        // downscaled - it would blow the per-frame upload budget and VRAM.
        const int EMBEDDED_TEXTURE_MAX_SIZE = static_cast<int>(embeddedTextureMaxSize());

        // Textura embutida ja' comprimida em bloco pelo bake.
        {
            auto bakedIt = bakedEmbedded.find(texPath);
            if (bakedIt != bakedEmbedded.end() && bakedIt->second.albedo.valid()) {
                MapContext::CpuModelTex cpuTex;
                cpuTex.path = texPath;
                cpuTex.etex = bakedIt->second.albedo;
                cpuTex.width = static_cast<int>(cpuTex.etex.width);
                cpuTex.height = static_cast<int>(cpuTex.etex.height);
                cpuTex.channels = 4;
                if (bakedIt->second.mrahw.valid() && bakedIt->second.normal.valid()) {
                    cpuTex.pbrMrahw.etex = bakedIt->second.mrahw;
                    cpuTex.pbrMrahw.width = static_cast<int>(cpuTex.pbrMrahw.etex.width);
                    cpuTex.pbrMrahw.height = static_cast<int>(cpuTex.pbrMrahw.etex.height);
                    cpuTex.pbrMrahw.channels = 4;
                    cpuTex.pbrNormal.etex = bakedIt->second.normal;
                    cpuTex.pbrNormal.width = static_cast<int>(cpuTex.pbrNormal.etex.width);
                    cpuTex.pbrNormal.height = static_cast<int>(cpuTex.pbrNormal.etex.height);
                    cpuTex.pbrNormal.channels = 4;
                }
                modelRamUsage.fetch_add(cpuTex.etex.byteSize()
                                        + cpuTex.pbrMrahw.etex.byteSize()
                                        + cpuTex.pbrNormal.etex.byteSize(),
                                        std::memory_order_relaxed);
                decodedTextures[t] = std::move(cpuTex);
                return;
            }
        }

        auto embIt = embeddedTextureMap.find(texPath);
        if (embIt != embeddedTextureMap.end()) {
            const EmbeddedTexture* et = embIt->second;
            if (!et->pixels.empty()) {
                MapContext::CpuModelTex cpuTex;
                cpuTex.path = texPath;
                cpuTex.pixels = et->pixels; // copy; decoded once per model
                cpuTex.width = et->width;
                cpuTex.height = et->height;
                cpuTex.channels = et->channels;
                ImageUtils::downscaleRGBA(cpuTex.width, cpuTex.height, cpuTex.pixels,
                                          EMBEDDED_TEXTURE_MAX_SIZE);
                modelRamUsage.fetch_add(cpuTex.pixels.size(), std::memory_order_relaxed);
                decodedTextures[t] = std::move(cpuTex);
                return;
            }
            if (!et->encodedData.empty()) {
                ImageData img = ImageUtils::loadFromMemoryRaw(et->encodedData.data(), et->encodedData.size());
                if (img.isValid()) {
                    MapContext::CpuModelTex cpuTex;
                    cpuTex.path = texPath;
                    cpuTex.pixels = std::move(img.pixels);
                    cpuTex.width = img.width;
                    cpuTex.height = img.height;
                    cpuTex.channels = img.channels;
                    ImageUtils::downscaleRGBA(cpuTex.width, cpuTex.height, cpuTex.pixels,
                                              EMBEDDED_TEXTURE_MAX_SIZE);
                    modelRamUsage.fetch_add(cpuTex.pixels.size(), std::memory_order_relaxed);
                    decodedTextures[t] = std::move(cpuTex);
                    return;
                }
            }
            return;
        }

        // Try filesystem assets first, then the legacy pack layouts.
        std::vector<std::string> searchPaths = {
            texPath,
            "data\\texture\\" + texPath,
            "data\\texture\\model\\" + texPath
        };
        size_t lastSlash = texPath.find_last_of('\\');
        std::string filename = (lastSlash != std::string::npos) ? texPath.substr(lastSlash + 1) : texPath;
        searchPaths.push_back("data\\texture\\model\\" + filename);
        searchPaths.push_back("data\\texture\\" + filename);

        std::vector<uint8_t> texData;
        std::string resolvedPath;
        for (const auto& p : searchPaths) {
            texData = loadTextureData(p, &resolvedPath);
            if (!texData.empty()) break;
        }

        if (!texData.empty()) {
            MapContext::CpuModelTex cpuTex;
            cpuTex.path = texPath;
            // Baked BC texture: skip decode + dilate entirely.
            cpuTex.etex = loadEtexForImage(resolvedPath);
            bool haveTexture = cpuTex.etex.valid();
            if (haveTexture) {
                cpuTex.width = static_cast<int>(cpuTex.etex.width);
                cpuTex.height = static_cast<int>(cpuTex.etex.height);
                cpuTex.channels = 4;
            } else {
                ImageData img = ImageUtils::loadFromMemory(texData.data(), texData.size());
                if (img.isValid()) {
                    cpuTex.pixels = std::move(img.pixels);
                    cpuTex.width = img.width;
                    cpuTex.height = img.height;
                    cpuTex.channels = img.channels;
                    haveTexture = true;
                }
            }
            if (haveTexture) {
                // Pre-decode PBR maps on the background thread.
                auto pbrIt = texPathToPbrName.find(texPath);
                const std::string& pbrLookup = (pbrIt != texPathToPbrName.end()) ? pbrIt->second : texPath;
                preloadPbrForAlbedo(texPath, pbrLookup, cpuTex.pbrMrahw, cpuTex.pbrNormal);
                // Embedded GLB textures have no file on disk, so neither the
                // cooked lookup nor the disk-based synthesis above can produce
                // anything - the material would render with flat fallback
                // constants. Synthesize from the decoded pixels we already hold.
                if ((!cpuTex.pbrMrahw.valid() || !cpuTex.pbrNormal.valid()) &&
                    !cpuTex.pixels.empty()) {
                    synthesizePbrFromPixels(cpuTex.pixels, cpuTex.width, cpuTex.height,
                                            cpuTex.channels > 0 ? cpuTex.channels : 4,
                                            cpuTex.pbrMrahw, cpuTex.pbrNormal);
                }

                modelRamUsage.fetch_add(cpuTex.pixels.size() + cpuTex.etex.byteSize()
                                        + cpuTex.pbrMrahw.pixels.size()
                                        + cpuTex.pbrNormal.pixels.size(), std::memory_order_relaxed);
                decodedTextures[t] = std::move(cpuTex);
            }
        }
    });

    if (m_cancelled.load()) return;
    PROFILE_RAM_ALLOC(ProfilerCategory::Textures, modelRamUsage.load());

    // Cache for model textures (path -> ctx.cpuModelTextures index)
    std::unordered_map<std::string, uint32_t> modelTextureCache;

    // Register decoded textures sequentially to get correct indices
    std::vector<uint32_t> globalTexToCtxIdx(allUniqueTextures.size(), UINT32_MAX);
    for (size_t t = 0; t < allUniqueTextures.size(); ++t) {
        const std::string& texPath = allUniqueTextures[t];
        auto pbrIt = texPathToPbrName.find(texPath);
        const std::string& pbrLookup = (pbrIt != texPathToPbrName.end()) ? pbrIt->second : std::string{};
        if (texCached[t]) {
            MapContext::CpuModelTex cpuTex;
            cpuTex.path = texPath;
            cpuTex.pbrName = pbrLookup;
            cpuTex.pixels = {}; // already on GPU
            // PBR maps were preloaded on the background thread for cached albedos.
            if (!decodedTextures[t].path.empty()) {
                cpuTex.pbrMrahw = std::move(decodedTextures[t].pbrMrahw);
                cpuTex.pbrNormal = std::move(decodedTextures[t].pbrNormal);
            }
            uint32_t idx = static_cast<uint32_t>(ctx.cpuModelTextures.size());
            ctx.cpuModelTextures.push_back(std::move(cpuTex));
            modelTextureCache[texPath] = idx;
            globalTexToCtxIdx[t] = idx;
        } else if (!decodedTextures[t].path.empty()) {
            decodedTextures[t].pbrName = pbrLookup;
            uint32_t idx = static_cast<uint32_t>(ctx.cpuModelTextures.size());
            ctx.cpuModelTextures.push_back(std::move(decodedTextures[t]));
            modelTextureCache[texPath] = idx;
            globalTexToCtxIdx[t] = idx;
        }
    }

    // Cache for model meshes (legacyModelPath -> list of indices in ctx.cpuModelMeshes)
    std::unordered_map<std::string, std::vector<uint32_t>> modelMeshIndexCache;

    // 3. Generate meshes for used source models.
    // Parallel over models (each writes only its own slot), and the texture
    // name -> ctx index resolution is hoisted into a per-node LUT instead of
    // re-doing string normalization + hash lookups PER VERTEX.
    struct ModelMeshBuild {
        std::vector<MapContext::CpuModelMesh> meshes;
        std::vector<uint32_t> meshIndices; // node -> local mesh idx (UINT32_MAX = none)
        size_t meshBytes = 0;
    };
    std::vector<ModelMeshBuild> meshBuilds(usedRsms.size());

    auto resolveTexName = [&](std::string tName) -> uint32_t {
        std::replace(tName.begin(), tName.end(), '/', '\\');
        if (!tName.empty() && tName[0] == '\\') tName = tName.substr(1);
        auto it_gid = texPathToGlobalIdx.find(tName);
        if (it_gid == texPathToGlobalIdx.end()) return UINT32_MAX;
        return globalTexToCtxIdx[it_gid->second];
    };

    eruption::JobSystem::instance().parallelFor(usedRsms.size(), [&](uint32_t mi) {
        if (m_cancelled.load()) return;
        const ModelFile& model = *usedRsms[mi];
        ModelMeshBuild& build = meshBuilds[mi];
        build.meshIndices.assign(model.nodes.size(), UINT32_MAX);

        for (size_t n = 0; n < model.nodes.size(); ++n) {
            const auto& node = model.nodes[n];
            if (node.vertices.empty() || node.indices.empty()) continue;

            // Per-node LUT: local texture slot -> ctx.cpuModelTextures index.
            std::vector<uint32_t> texLut;
            if (!node.textureNames.empty()) {
                // v2.3+: per-node names
                texLut.resize(node.textureNames.size());
                for (size_t j = 0; j < node.textureNames.size(); ++j)
                    texLut[j] = resolveTexName(node.textureNames[j]);
            } else if (!node.textureIds.empty()) {
                // v1.x-2.2: map local ID to global list
                texLut.resize(node.textureIds.size());
                for (size_t j = 0; j < node.textureIds.size(); ++j) {
                    uint32_t modelIdx = node.textureIds[j];
                    texLut[j] = (modelIdx < model.textures.size())
                                    ? resolveTexName(model.textures[modelIdx])
                                    : UINT32_MAX;
                }
            } else {
                // Direct mapping
                texLut.resize(model.textures.size());
                for (size_t j = 0; j < model.textures.size(); ++j)
                    texLut[j] = resolveTexName(model.textures[j]);
            }

            MapContext::CpuModelMesh cpuMesh;
            cpuMesh.nodeIndex = static_cast<uint32_t>(n);
            cpuMesh.legacyModelPath = model.filePath;
            cpuMesh.vertices.reserve(node.vertices.size());

            Vec3 aabbMin(FLT_MAX);
            Vec3 aabbMax(-FLT_MAX);

            for (size_t i = 0; i < node.vertices.size(); ++i) {
                uint16_t faceTexIdx = (i < node.perVertexTexIds.size()) ? node.perVertexTexIds[i] : 0;
                uint32_t cpuTexIdx = texLut.empty()
                    ? UINT32_MAX
                    : ((!node.textureNames.empty() || !node.textureIds.empty())
                           ? texLut[faceTexIdx % texLut.size()]
                           : (faceTexIdx < texLut.size() ? texLut[faceTexIdx] : UINT32_MAX));

                // GLB-authored texture crossfade target (async/warp path):
                // resolve to a CPU texture index the same way as the primary.
                // Inert when the node carries no _BLEND_WEIGHT data.
                float blendWeight = (i < node.perVertexBlendWeights.size())
                                        ? node.perVertexBlendWeights[i] : 0.0f;
                uint32_t cpuBlendIdx = UINT32_MAX;
                if (blendWeight > 0.0f && i < node.perVertexBlendTexIds.size() &&
                    !node.textureNames.empty()) {
                    uint16_t bLocal = static_cast<uint16_t>(
                        node.perVertexBlendTexIds[i] % node.textureNames.size());
                    cpuBlendIdx = texLut[bLocal];
                }

                TerrainVertex v{};
                v.position = node.vertices[i];
                v.texCoord = node.texCoords[i];
                v.normal = node.normals[i];
                v.texIndex = cpuTexIdx;
                v.matId = 0;
                v.color = (i < node.colors.size()) ? node.colors[i] : 0xFFFFFFFF;
                if (cpuBlendIdx != UINT32_MAX) {
                    v.blendTexIndex = cpuBlendIdx;
                    v.blendPbrIndex = cpuBlendIdx;
                    v.blendNormalIndex = cpuBlendIdx;
                    v.blendWeight = blendWeight;
                }
                cpuMesh.vertices.push_back(v);

                aabbMin = glm::min(aabbMin, v.position);
                aabbMax = glm::max(aabbMax, v.position);
            }

            cpuMesh.indices = node.indices;
            cpuMesh.aabbMin = aabbMin;
            cpuMesh.aabbMax = aabbMax;

            build.meshBytes += cpuMesh.vertices.size() * sizeof(TerrainVertex)
                             + cpuMesh.indices.size() * sizeof(uint32_t);
            build.meshIndices[n] = static_cast<uint32_t>(build.meshes.size());
            build.meshes.push_back(std::move(cpuMesh));
        }
    });
    if (m_cancelled.load()) return;

    // Serial merge preserving the original ctx.cpuModelMeshes ordering.
    for (size_t mi = 0; mi < usedRsms.size(); ++mi) {
        const ModelFile& model = *usedRsms[mi];
        ModelMeshBuild& build = meshBuilds[mi];
        const uint32_t base = static_cast<uint32_t>(ctx.cpuModelMeshes.size());
        for (auto& idx : build.meshIndices) {
            if (idx != UINT32_MAX) idx += base;
        }
        // Nós que compartilham geometria (geometryShareIndex >= 0) não
        // construíram malha: apontam para a do dono. Feito depois do += base
        // para já usar o índice global. Ver GltfParser::meshGeometryOwner.
        for (size_t n = 0; n < build.meshIndices.size(); ++n) {
            if (build.meshIndices[n] != UINT32_MAX) continue;
            int32_t src = model.nodes[n].geometryShareIndex;
            if (src >= 0 && src < static_cast<int32_t>(build.meshIndices.size())) {
                build.meshIndices[n] = build.meshIndices[src];
            }
        }
        for (auto& mesh : build.meshes) {
            ctx.cpuModelMeshes.push_back(std::move(mesh));
        }
        PROFILE_RAM_ALLOC(ProfilerCategory::Meshes, build.meshBytes);
        modelMeshIndexCache[model.filePath] = build.meshIndices;
        ctx.gpuModelMeshCache[model.filePath] = build.meshIndices;
    }

    // 4. Create instances using the cached mesh indices.
    // For GLB maps the entire scene is already authored in world space, so each
    // ModelFile becomes a single identity instance and node.renderMatrix already
    // contains the authored transform.
    for (const ModelFile* modelPtr : usedRsms) {
        if (m_cancelled.load()) return;

        const ModelFile& model = *modelPtr;
        if (model.nodes.empty()) continue;

        const auto& meshIndices_model = modelMeshIndexCache[model.filePath];
        if (meshIndices_model.empty()) continue;

        // Identity placement: GLB vertices/nodes are already in world space.
        Mat4 instanceMatrix = Mat4(1.0f);
        Mat4 mainOffset = Mat4(1.0f);
        Vec3 scale(1.0f);

        for (size_t n = 0; n < model.nodes.size(); ++n) {
            const auto& node = model.nodes[n];
            uint32_t mIdx = meshIndices_model[n];
            if (mIdx == UINT32_MAX) continue;

            ModelInstance inst{};
            inst.meshIndex = mIdx;
            inst.transform = instanceMatrix * mainOffset * node.renderMatrix;
            inst.enabled = true;
            inst.name = model.filePath + " (" + node.name + ")";
            inst.assetPath = model.filePath;
            inst.nodeIndex = static_cast<uint32_t>(n);

            // Calculate world-space AABB for culling
            if (inst.meshIndex < ctx.cpuModelMeshes.size()) {
                const auto& meshRef = ctx.cpuModelMeshes[inst.meshIndex];
                Vec3 corners[8] = {
                    meshRef.aabbMin,
                    {meshRef.aabbMax.x, meshRef.aabbMin.y, meshRef.aabbMin.z},
                    {meshRef.aabbMin.x, meshRef.aabbMax.y, meshRef.aabbMin.z},
                    {meshRef.aabbMax.x, meshRef.aabbMax.y, meshRef.aabbMin.z},
                    {meshRef.aabbMin.x, meshRef.aabbMin.y, meshRef.aabbMax.z},
                    {meshRef.aabbMax.x, meshRef.aabbMin.y, meshRef.aabbMax.z},
                    {meshRef.aabbMax.x, meshRef.aabbMax.y, meshRef.aabbMax.z},
                    meshRef.aabbMax
                };

                Vec3 wMin(FLT_MAX);
                Vec3 wMax(-FLT_MAX);
                for (int i = 0; i < 8; ++i) {
                    Vec3 p = Vec3(inst.transform * Vec4(corners[i], 1.0f));
                    wMin = glm::min(wMin, p);
                    wMax = glm::max(wMax, p);
                }
                inst.worldAabbMin = wMin;
                inst.worldAabbMax = wMax;
                inst.worldCenter = (wMin + wMax) * 0.5f;

                // Estimate world-space bounding radius
                Vec3 localSize = meshRef.aabbMax - meshRef.aabbMin;
                inst.boundingRadius = glm::length(localSize) * 0.5f;
            } else {
                inst.boundingRadius = 10.0f;
            }

            // Detect animated nodes
            bool isAnimated = node.scaleKeyframes.size() >= 2 || node.rotKeyframes.size() >= 2 || node.posKeyframes.size() >= 2;
            if (isAnimated) {
                ModelRenderer::AnimatedNodeData anim;
                anim.modelAssetIndex = modelToAssetIdx[&model];
                anim.nodeIndex = static_cast<uint32_t>(n);
                anim.instanceMatrix = instanceMatrix;
                anim.mainOffset = mainOffset;
                anim.instanceIndex = static_cast<uint32_t>(ctx.cpuModelInstances.size());
                ctx.cpuAnimatedNodes.push_back(std::move(anim));
            }
            ctx.cpuModelInstances.push_back(inst);
        }
    }
}

} // namespace eruption
