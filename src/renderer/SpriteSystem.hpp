#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/SpriteRenderer.hpp"
#include "math/Frustum.hpp"
#include "math/Types.hpp"
#include <vector>
#include <queue>
#include <unordered_map>
#include <algorithm>

namespace eruption {

enum class SpriteFlags : uint32_t {
    None        = 0,
    FlipX       = 1 << 0,
    FlipY       = 1 << 1,
    Billboard   = 1 << 2,
    // Bits 3-7 reserved for MaterialId in shaders
    UsePalette  = 1 << 8,
    CastShadow  = 1 << 9,
    NoDepthSort = 1 << 10,
    PngSprite   = 1 << 11,
};

inline SpriteFlags operator|(SpriteFlags a, SpriteFlags b) {
    return static_cast<SpriteFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline SpriteFlags operator&(SpriteFlags a, SpriteFlags b) {
    return static_cast<SpriteFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool hasFlag(SpriteFlags flags, SpriteFlags flag) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

struct Sprite {
    Vec3 position = Vec3(0.0f);
    Vec3 anchorPoint = Vec3(0.0f); // Shared center for multi-part billboarding
    Vec2 size = Vec2(1.0f);
    uint32_t texIndex = 0;
    uint32_t normalTexIndex = 0;
    uint32_t mrahwTexIndex = 0;
    uint32_t paletteIndex = 0;
    Vec4 texRect = Vec4(0.0f);
    Vec4 tintColor = Vec4(1.0f);
    SpriteFlags flags = SpriteFlags::None;
    uint32_t materialId = 0;
    float sortOrder = 0.0f;

    AABB getAABB() const {
        Vec3 halfSize(size.x * 0.5f, size.y * 0.5f, 0.01f);
        return AABB{position - halfSize, position + halfSize};
    }
};

// Subconjunto de Sprite (92B) usado pelo grafo de sobreposicao de
// topologicalSort: so' o que isBehind()/getAABB() precisam (24B).
//
// Ofensor #2 do relatorio de memoria: o grafo de adjacencia itera por BUCKET
// de hash espacial - ordem NAO sequencial, o prefetcher de hardware nao
// acompanha - e cada vizinho testado tocava um Sprite inteiro (92B, ~2 cache
// lines) so' para ler ~20B (posicao+tamanho+flags). texRect/tintColor/
// texIndex/paletteIndex/materialId nunca entram no teste de sobreposicao;
// so' servem no upload final para a GPU (uploadInstances()).
struct SpriteHot {
    Vec3 position;
    Vec2 size;
    SpriteFlags flags;
};

struct SpriteBatch {
    uint32_t texIndex;
    uint32_t startIndex;
    uint32_t count;
};

class SpriteSystem {
public:
    void init(VulkanContext* ctx, SpriteRenderer* renderer, uint32_t maxSprites = 10000);
    void shutdown();

    void submitSprite(const Sprite& sprite);
    void submitSprites(const std::vector<Sprite>& sprites);
    void clearSprites() { m_submittedSprites.clear(); }

    void processSprites(const Frustum& frustum, const Vec3& cameraPos,
                        const Vec3& cameraForward);

    void render(VkCommandBuffer cmd);

    uint32_t totalSubmitted() const { return m_submittedCount; }
    uint32_t visibleCount() const { return m_visibleCount; }
    uint32_t drawCallCount() const { return m_drawCallCount; }
    VkBuffer instanceBuffer() const { return m_instanceBuffer; }
    const std::vector<Sprite>& getVisibleSprites() const { return m_visibleSprites; }
    // Ordem final de desenho (pos-topologicalSort). Simetrico a
    // getVisibleSprites() - existia so' internamente; exposto para
    // verificacao/tooling (o teste do Ofensor #2 usa isto para comparar a
    // ordem exata antes/depois da reestruturacao Hot/Gpu).
    const std::vector<Sprite>& getSortedSprites() const { return m_sortedSprites; }

    void setDepthSortingEnabled(bool enabled) { m_depthSortEnabled = enabled; }

private:
    VulkanContext* m_ctx = nullptr;
    SpriteRenderer* m_renderer = nullptr;

    uint32_t m_maxSprites;
    std::vector<Sprite> m_submittedSprites;
    std::vector<Sprite> m_visibleSprites;
    std::vector<Sprite> m_sortedSprites;

    VkBuffer m_instanceBuffer = VK_NULL_HANDLE;
    VmaAllocation m_instanceAlloc = VK_NULL_HANDLE;
    void* m_mappedInstances = nullptr;

    struct SortKey {
        float depth;
        uint32_t index;
    };
    std::vector<SortKey> m_sortKeys;
    bool m_depthSortEnabled = true;

    // Scratch de topologicalSort. Vetores membros (nao locais) para nao
    // realocar por frame - mesmo padrao ja' usado por m_sortKeys.
    std::vector<SpriteHot> m_hotScratch;
    std::vector<uint32_t> m_orderScratch;
    // Scratch CSR do topologicalSort (grade espacial + grafo + Kahn). Membros
    // para nao alocar por frame: a versao com vector-por-celula e
    // vector-por-sprite fazia milhares de malloc a cada quadro.
    std::unordered_map<uint64_t, uint32_t> m_cellSlot;  // chave da celula -> id denso
    std::vector<uint32_t> m_cellOfSprite;
    std::vector<uint32_t> m_cellStart, m_cellItems;
    std::vector<uint32_t> m_graphStart, m_graphEdges;
    std::vector<uint32_t> m_inDegree, m_topoOrder, m_finalOrder;
    std::vector<uint32_t> m_scratchCursor;

    uint32_t m_submittedCount = 0;
    uint32_t m_visibleCount = 0;
    uint32_t m_drawCallCount = 0;

    void frustumCull(const Frustum& frustum);
    void topologicalSort(const Vec3& cameraPos);
    void buildBatches();
    void uploadInstances();

    static bool isBehind(const SpriteHot& a, const SpriteHot& b);
    static AABB aabbOf(const SpriteHot& s);
};

} // namespace eruption
