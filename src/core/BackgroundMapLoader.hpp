#pragma once

#include "renderer/MapContext.hpp"
#include "formats/MapLoader.hpp"

#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include <future>
#include <thread>

namespace eruption {

class Engine; // Forward declaration

// ------------------------------------------------------------------
// BackgroundMapLoader runs I/O + parsing on a background thread.
// It produces a MapContext whose CPU data is fully prepared.
// The main thread then uploads GPU resources incrementally.
// ------------------------------------------------------------------
class BackgroundMapLoader {
public:
    // We pass a pointer to Engine's asset cache to allow skipping existing resources
    struct AssetCacheInterface {
        std::function<bool(const std::string&)> hasModelTexture;
        std::function<bool(const std::string&)> hasTerrainTexture;
    };

    BackgroundMapLoader(AssetCacheInterface cache = {});

    // Start loading a map asynchronously.
    // Returns false if a load is already in progress.
    ~BackgroundMapLoader();

    bool startLoad(const std::string& mapName);

    // Cancel any in-progress load.
    void cancel();

    // Poll for completion. Returns nullptr if not finished yet.
    std::unique_ptr<MapContext> pollResult();

    // Blocking wait for completion.
    std::unique_ptr<MapContext> waitResult();

    float progress() const { return m_progress.load(); }
    bool isLoading() const { return m_loading.load(); }
    const std::string& currentMapName() const { return m_currentMapName; }

private:
    std::unique_ptr<MapContext> loadJob(const std::string& mapName);

    // Helpers that fill CPU data into the context
    void prepareTerrainData(MapContext& ctx);
    void prepareModelData(MapContext& ctx);

    // Texture lookup: prefer filesystem assets/data/texture, then pack archive.
    // outResolvedPath (optional) receives the filesystem path the bytes came
    // from, letting callers look up the baked .etex sibling.
    std::vector<uint8_t> loadTextureData(const std::string& texPath,
                                         std::string* outResolvedPath = nullptr);

    // Decode pre-cooked PBR maps for an albedo path on the background thread.
    // pbrName is the original material name (for GLB assets) which matches the
    // cooked _mrahw/_normal filenames better than the synthetic embedded image name.
    void preloadPbrForAlbedo(const std::string& albedoPath,
                             const std::string& pbrName,
                             PbrTextureData& outMrahw,
                             PbrTextureData& outNormal);

private:
    std::unique_ptr<MapLoader> m_mapLoader;
    AssetCacheInterface m_cache;

    std::atomic<bool> m_loading{false};
    std::atomic<bool> m_cancelled{false};
    std::atomic<float> m_progress{0.0f};
    std::string m_currentMapName;

    std::thread m_thread;
    std::promise<std::unique_ptr<MapContext>> m_promise;
    std::future<std::unique_ptr<MapContext>> m_future;
};

} // namespace eruption
