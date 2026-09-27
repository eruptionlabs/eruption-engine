#pragma once

#include "math/Frustum.hpp"
#include "math/Types.hpp"
#include <vector>

namespace eruption {

struct Chunk {
    AABB bounds;
    uint32_t startVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t startIndex = 0;
    uint32_t indexCount = 0;
    bool visible = false;
    bool dirty = true;

    float isoDepth() const {
        Vec3 c = bounds.center();
        return c.x + c.y;
    }
};

class ChunkManager {
public:
    void init(uint32_t mapWidth, uint32_t mapHeight, uint32_t chunkSize);

    void updateVisibility(const Frustum& frustum);
    std::vector<const Chunk*> getVisibleChunks() const;

    uint32_t chunkCount() const { return static_cast<uint32_t>(m_chunks.size()); }
    uint32_t visibleCount() const;

private:
    uint32_t m_mapWidth = 0;
    uint32_t m_mapHeight = 0;
    uint32_t m_chunkSize = 0;
    std::vector<Chunk> m_chunks;

    uint32_t m_gridWidth = 0;
    uint32_t m_gridHeight = 0;
    std::vector<std::vector<uint32_t>> m_spatialGrid;

    uint32_t getGridCell(float x, float y) const;
};

} // namespace eruption
