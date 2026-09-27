#include "renderer/ChunkManager.hpp"
#include <algorithm>

namespace eruption {

void ChunkManager::init(uint32_t mapWidth, uint32_t mapHeight, uint32_t chunkSize) {
    m_mapWidth = mapWidth;
    m_mapHeight = mapHeight;
    m_chunkSize = chunkSize;

    uint32_t chunksX = (mapWidth + chunkSize - 1) / chunkSize;
    uint32_t chunksY = (mapHeight + chunkSize - 1) / chunkSize;
    m_chunks.resize(chunksX * chunksY);

    for (uint32_t cy = 0; cy < chunksY; cy++) {
        for (uint32_t cx = 0; cx < chunksX; cx++) {
            uint32_t idx = cy * chunksX + cx;
            Chunk& chunk = m_chunks[idx];
            float minX = static_cast<float>(cx * chunkSize);
            float minZ = static_cast<float>(cy * chunkSize);
            float maxX = static_cast<float>(std::min((cx + 1) * chunkSize, mapWidth));
            float maxZ = static_cast<float>(std::min((cy + 1) * chunkSize, mapHeight));
            chunk.bounds = AABB{Vec3(minX, -10.0f, minZ), Vec3(maxX, 10.0f, maxZ)};
        }
    }

    m_gridWidth = chunksX;
    m_gridHeight = chunksY;
    m_spatialGrid.resize(m_gridWidth * m_gridHeight);
    for (uint32_t i = 0; i < m_chunks.size(); i++) {
        m_spatialGrid[i].push_back(i);
    }
}

void ChunkManager::updateVisibility(const Frustum& frustum) {
    for (auto& chunk : m_chunks) {
        chunk.visible = frustum.intersectsAABB(chunk.bounds);
    }
}

std::vector<const Chunk*> ChunkManager::getVisibleChunks() const {
    std::vector<const Chunk*> visible;
    visible.reserve(m_chunks.size());
    for (const auto& chunk : m_chunks) {
        if (chunk.visible) visible.push_back(&chunk);
    }
    std::sort(visible.begin(), visible.end(),
              [](const Chunk* a, const Chunk* b) { return a->isoDepth() < b->isoDepth(); });
    return visible;
}

uint32_t ChunkManager::visibleCount() const {
    uint32_t count = 0;
    for (const auto& chunk : m_chunks) {
        if (chunk.visible) count++;
    }
    return count;
}

uint32_t ChunkManager::getGridCell(float x, float y) const {
    int cx = static_cast<int>(x / m_chunkSize);
    int cy = static_cast<int>(y / m_chunkSize);
    cx = std::max(0, std::min(cx, static_cast<int>(m_gridWidth) - 1));
    cy = std::max(0, std::min(cy, static_cast<int>(m_gridHeight) - 1));
    return cy * m_gridWidth + cx;
}

} // namespace eruption
