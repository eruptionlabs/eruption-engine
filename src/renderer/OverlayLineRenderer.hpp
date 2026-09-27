#pragma once

#include "renderer/VulkanContext.hpp"
#include "math/Types.hpp"

#include <vector>

namespace eruption {

struct OverlayLineVertex {
    Vec3 position;
    Vec3 color;
};

// Line renderer for world-space overlays (gizmos, debug shapes).
class OverlayLineRenderer {
public:
    bool init(VulkanContext* ctx);
    void shutdown();

    void addLine(const Vec3& a, const Vec3& b, const Vec3& color);
    void addBox(const Vec3& min, const Vec3& max, const Vec3& color);
    void clearLines();
    size_t lineCount() const { return m_lines.size(); }

    void render(VkCommandBuffer cmd, const Mat4& viewProj);
    void upload();

    bool isInitialized() const { return m_initialized; }

private:
    VulkanContext* m_ctx = nullptr;

    std::vector<OverlayLineVertex> m_lines;

    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_vertexAlloc = VK_NULL_HANDLE;
    VkDeviceSize m_vertexBufferSize = 0;

    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

    bool m_initialized = false;
    bool m_dirty = true;

    void createPipeline();
};

} // namespace eruption
