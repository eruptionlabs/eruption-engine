#include "renderer/SpriteSystem.hpp"
#include "core/Logger.hpp"
#include "utils/Profiler.hpp"
#include <cstring>

namespace eruption {

void SpriteSystem::init(VulkanContext* ctx, SpriteRenderer* renderer, uint32_t maxSprites) {
    m_ctx = ctx;
    m_renderer = renderer;
    m_maxSprites = maxSprites;
    m_submittedSprites.reserve(maxSprites);
    m_visibleSprites.reserve(maxSprites);
    m_sortedSprites.reserve(maxSprites);
    m_sortKeys.reserve(maxSprites);

    VkDeviceSize instanceSize = maxSprites * sizeof(SpriteInstanceData);
    if (ctx->createBuffer(instanceSize,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VMA_MEMORY_USAGE_CPU_TO_GPU,
                          m_instanceBuffer, m_instanceAlloc)) {
        vmaMapMemory(ctx->allocator(), m_instanceAlloc, &m_mappedInstances);
    }
}

void SpriteSystem::shutdown() {
    if (m_instanceBuffer != VK_NULL_HANDLE) {
        vmaUnmapMemory(m_ctx->allocator(), m_instanceAlloc);
        vmaDestroyBuffer(m_ctx->allocator(), m_instanceBuffer, m_instanceAlloc);
        m_instanceBuffer = VK_NULL_HANDLE;
    }
}

void SpriteSystem::submitSprite(const Sprite& sprite) {
    if (m_submittedSprites.size() < m_maxSprites) {
        m_submittedSprites.push_back(sprite);
    }
}

void SpriteSystem::submitSprites(const std::vector<Sprite>& sprites) {
    for (const auto& s : sprites) {
        submitSprite(s);
    }
}

void SpriteSystem::processSprites(const Frustum& frustum, const Vec3& cameraPos,
                                  const Vec3& cameraForward) {
    PROFILE_CPU_SCOPE(ProfilerCategory::Culling);
    m_submittedCount = static_cast<uint32_t>(m_submittedSprites.size());
    frustumCull(frustum);
    topologicalSort(cameraPos);
    buildBatches();
    uploadInstances();
    m_submittedSprites.clear();
}

void SpriteSystem::frustumCull(const Frustum& frustum) {
    m_visibleSprites.clear();
    m_visibleSprites.reserve(m_submittedSprites.size());
    for (const auto& s : m_submittedSprites) {
        AABB box = s.getAABB();
        if (box.isValid()) {
            // Blob/planar shadows draw under the sprite: extend the box down
            // so a sprite above the view keeps its ground shadow alive
            // ("se tem sombra, mantenha").
            box.min.y -= 40.0f;
            if (!frustum.intersectsAABB(box)) continue;
        }
        m_visibleSprites.push_back(s);
    }
    m_visibleCount = static_cast<uint32_t>(m_visibleSprites.size());
}

void SpriteSystem::topologicalSort(const Vec3& cameraPos) {
    if (!m_depthSortEnabled || m_visibleSprites.size() < 2) {
        m_sortedSprites = m_visibleSprites;
        return;
    }

    const uint32_t n = static_cast<uint32_t>(m_visibleSprites.size());

    // SpriteHot (24B) em vez de Sprite (92B) para tudo que so' precisa
    // decidir ORDEM. So' a materializacao final (fim da funcao) volta a
    // tocar o Sprite completo - Ofensor #2 do relatorio de memoria.
    m_hotScratch.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        const Sprite& s = m_visibleSprites[i];
        m_hotScratch[i] = SpriteHot{s.position, s.size, s.flags};
    }

    m_sortKeys.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        const SpriteHot& h = m_hotScratch[i];
        float isoDepth = h.position.x + h.position.y + h.position.z * 0.001f;
        // sortOrder nao esta' no Hot (so' e' usado aqui, uma vez, sequencial -
        // nao compensa duplicar no struct pequeno so' por causa deste calculo).
        m_sortKeys[i] = {isoDepth + m_visibleSprites[i].sortOrder, i};
    }

    std::sort(m_sortKeys.begin(), m_sortKeys.end(),
              [](const SortKey& a, const SortKey& b) { return a.depth < b.depth; });

    // `order[k]` = indice em m_visibleSprites do k-esimo sprite na ordem
    // atual. Do inicio ao fim da funcao, so' INDICES sao permutados; nenhum
    // Sprite completo e' copiado ate' a materializacao final.
    m_orderScratch.resize(n);
    for (uint32_t i = 0; i < n; ++i) m_orderScratch[i] = m_sortKeys[i].index;

    // Reordena o Hot para acompanhar `order` - o grafo abaixo usa daqui em
    // diante, na mesma ordem por profundidade que o codigo original usava
    // (o teste `jIdx <= i` depende dessa ordem).
    {
        std::vector<SpriteHot> hotOrdered(n);
        for (uint32_t i = 0; i < n; ++i) hotOrdered[i] = m_hotScratch[m_orderScratch[i]];
        m_hotScratch.swap(hotOrdered);
    }

    // Topological sort with spatial hashing for overlapping sprites
    //
    // TUDO EM CSR (varredura 2026-09-04, pendencia 11 do docs/pedidos.md).
    // A versao anterior alocava, POR FRAME: um std::vector por celula do hash
    // espacial, mais um std::vector POR SPRITE para as arestas do grafo
    // (`vector<vector<uint32_t>> graph(n)`), mais inDegree/queue/topoOrder/
    // finalOrder. Com n na casa dos milhares isso e' milhares de malloc por
    // frame, e cada lista de adjacencia cai num lugar diferente da memoria -
    // o percurso do grafo vira ponteiro-perseguicao.
    // Agora: dois arrays planos (inicio + itens) para a grade e outros dois
    // para o grafo, todos MEMBROS reusados entre frames. A ORDEM de visita e'
    // identica a de antes (mesmo laco i, mesmas celulas, mesmo criterio), e as
    // arestas de cada no' saem na mesma ordem relativa - o resultado do Kahn
    // e', portanto, o mesmo.
    if (n > 100) {
        const float CELL_SIZE = 2.0f;
        auto cellKey = [](uint32_t cx, uint32_t cy) {
            return (static_cast<uint64_t>(cx) << 32) | cy;
        };

        // --- grade espacial em CSR ---
        m_cellSlot.clear();
        m_cellOfSprite.resize(n);
        uint32_t numCells = 0;
        for (uint32_t i = 0; i < n; i++) {
            const SpriteHot& h = m_hotScratch[i];
            const uint64_t key = cellKey(static_cast<uint32_t>(h.position.x / CELL_SIZE),
                                         static_cast<uint32_t>(h.position.y / CELL_SIZE));
            auto it = m_cellSlot.find(key);
            if (it == m_cellSlot.end()) it = m_cellSlot.emplace(key, numCells++).first;
            m_cellOfSprite[i] = it->second;
        }
        m_cellStart.assign(numCells + 1, 0);
        for (uint32_t i = 0; i < n; i++) ++m_cellStart[m_cellOfSprite[i] + 1];
        for (uint32_t c = 0; c < numCells; ++c) m_cellStart[c + 1] += m_cellStart[c];
        m_cellItems.resize(n);
        {
            std::vector<uint32_t>& cursor = m_scratchCursor;
            cursor.assign(m_cellStart.begin(), m_cellStart.end() - 1);
            for (uint32_t i = 0; i < n; i++) m_cellItems[cursor[m_cellOfSprite[i]]++] = i;
        }

        // --- grafo em CSR: passe 1 conta, passe 2 preenche ---
        // O par (i,j) gera aresta em i OU em j, entao os dois passes tem que
        // varrer exatamente a mesma sequencia de pares.
        m_graphStart.assign(n + 1, 0);
        auto forEachEdge = [&](auto&& emit) {
            for (uint32_t i = 0; i < n; i++) {
                const SpriteHot& hi = m_hotScratch[i];
                if (hasFlag(hi.flags, SpriteFlags::NoDepthSort)) continue;
                const uint32_t cx = static_cast<uint32_t>(hi.position.x / CELL_SIZE);
                const uint32_t cy = static_cast<uint32_t>(hi.position.y / CELL_SIZE);
                for (int dx = -1; dx <= 1; dx++) {
                    for (int dy = -1; dy <= 1; dy++) {
                        auto it = m_cellSlot.find(cellKey(cx + dx, cy + dy));
                        if (it == m_cellSlot.end()) continue;
                        const uint32_t c = it->second;
                        for (uint32_t k = m_cellStart[c]; k < m_cellStart[c + 1]; ++k) {
                            const uint32_t jIdx = m_cellItems[k];
                            if (jIdx <= i) continue;
                            const SpriteHot& hj = m_hotScratch[jIdx];
                            if (hasFlag(hj.flags, SpriteFlags::NoDepthSort)) continue;
                            if (isBehind(hi, hj)) emit(i, jIdx);
                            else if (isBehind(hj, hi)) emit(jIdx, i);
                        }
                    }
                }
            }
        };
        forEachEdge([&](uint32_t u, uint32_t) { ++m_graphStart[u + 1]; });
        for (uint32_t i = 0; i < n; ++i) m_graphStart[i + 1] += m_graphStart[i];
        m_graphEdges.resize(m_graphStart[n]);
        {
            std::vector<uint32_t>& cursor = m_scratchCursor;
            cursor.assign(m_graphStart.begin(), m_graphStart.end() - 1);
            forEachEdge([&](uint32_t u, uint32_t v) { m_graphEdges[cursor[u]++] = v; });
        }

        // --- Kahn, com fila FIFO num array (mesma ordem do std::queue) ---
        m_inDegree.assign(n, 0);
        for (uint32_t e = 0; e < m_graphEdges.size(); ++e) ++m_inDegree[m_graphEdges[e]];
        m_topoOrder.clear();
        m_topoOrder.reserve(n);
        for (uint32_t i = 0; i < n; i++) if (m_inDegree[i] == 0) m_topoOrder.push_back(i);
        for (size_t head = 0; head < m_topoOrder.size(); ++head) {
            const uint32_t u = m_topoOrder[head];
            for (uint32_t e = m_graphStart[u]; e < m_graphStart[u + 1]; ++e) {
                const uint32_t v = m_graphEdges[e];
                if (--m_inDegree[v] == 0) m_topoOrder.push_back(v);
            }
        }

        if (m_topoOrder.size() == n) {
            // topoOrder indexa a ordem-por-profundidade; traduz de volta para
            // indices em m_visibleSprites via `order` (que ainda guarda essa
            // ordem, ja' que so' reordenamos o Hot, nao o `order` em si).
            m_finalOrder.resize(n);
            for (uint32_t i = 0; i < n; ++i) m_finalOrder[i] = m_orderScratch[m_topoOrder[i]];
            m_orderScratch.swap(m_finalOrder);
        }
    }

    // UNICA materializacao de Sprite (92B) completo desta funcao - antes
    // podia acontecer ate' DUAS vezes (gather por profundidade + gather do
    // ramo topologico).
    m_sortedSprites.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        m_sortedSprites[i] = m_visibleSprites[m_orderScratch[i]];
    }
}

AABB SpriteSystem::aabbOf(const SpriteHot& s) {
    Vec3 halfSize(s.size.x * 0.5f, s.size.y * 0.5f, 0.01f);
    return AABB{s.position - halfSize, s.position + halfSize};
}

bool SpriteSystem::isBehind(const SpriteHot& a, const SpriteHot& b) {
    AABB aAABB = aabbOf(a);
    AABB bAABB = aabbOf(b);
    if (!aAABB.isValid() || !bAABB.isValid()) return false;
    // Simple overlap test in XZ
    bool overlapX = (aAABB.min.x <= bAABB.max.x) && (aAABB.max.x >= bAABB.min.x);
    bool overlapZ = (aAABB.min.z <= bAABB.max.z) && (aAABB.max.z >= bAABB.min.z);
    if (!overlapX || !overlapZ) return false;
    return a.position.y < b.position.y && a.position.x < b.position.x;
}

void SpriteSystem::buildBatches() {
    // Sprites are already sorted by depth; batches group by texture
    m_drawCallCount = 0;
}

void SpriteSystem::uploadInstances() {
    if (!m_mappedInstances || m_sortedSprites.empty()) return;

    uint32_t count = static_cast<uint32_t>(m_sortedSprites.size());
    count = std::min(count, m_maxSprites);

    SpriteInstanceData* instances = reinterpret_cast<SpriteInstanceData*>(m_mappedInstances);
    for (uint32_t i = 0; i < count; i++) {
        const Sprite& s = m_sortedSprites[i];
        SpriteInstanceData& inst = instances[i];
        // Build model matrix
        Mat4 model = Mat4(1.0f);
        model = glm::translate(model, s.position);
        model = glm::scale(model, Vec3(s.size.x, s.size.y, 1.0f));
        inst.modelMatrix = model;
        inst.anchorPoint = Vec4(s.anchorPoint, s.sortOrder);
        inst.texRect = s.texRect;
        inst.texIndex = s.texIndex;
        inst.normalTexIndex = s.normalTexIndex;
        inst.mrahwTexIndex = s.mrahwTexIndex;

        uint32_t flags = static_cast<uint32_t>(s.flags);
        // Bit 8 is reserved for usePalette in some parts, but here we set MatId in 3-7
        flags |= (s.materialId & 0x1F) << 3; 
        inst.flags = flags;
        
        inst.paletteIndex = s.paletteIndex;
        inst.tintColor = s.tintColor;
    }

    m_drawCallCount = 1; // Single instanced draw
    
    // Flush to ensure GPU visibility (especially on discrete GPUs)
    vmaFlushAllocation(m_ctx->allocator(), m_instanceAlloc, 0, count * sizeof(SpriteInstanceData));
}

void SpriteSystem::render(VkCommandBuffer cmd) {
    if (m_sortedSprites.empty()) return;

    uint32_t count = static_cast<uint32_t>(m_sortedSprites.size());
    count = std::min(count, m_maxSprites);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 1, 1, &m_instanceBuffer, &offset);
    // The renderer handles the actual draw; we just prepare the instance buffer
}

} // namespace eruption
