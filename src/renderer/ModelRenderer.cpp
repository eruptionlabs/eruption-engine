#include "renderer/GBuffer.hpp"
#include <cfloat>
#include <cmath>
#include "renderer/ModelRenderer.hpp"

#include <cctype>

#include <unordered_map>
#include "utils/MeshLod.hpp"
#include <chrono>
#include <unordered_set>
#include <cstdlib>
#include <string>
#include "renderer/PipelineBuilder.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "formats/TerrainParser.hpp" // for TerrainVertex
#include "core/Logger.hpp"
#include "utils/ImageUtils.hpp"
#include "utils/Profiler.hpp"

#include <cstring>
#include <algorithm>
#include <fstream>
#include <mutex>

namespace eruption {

// Piso de area (fracao da copa original) que o LOD DISTANTE de malha de
// cartao pode descer - ver o comentario na chamada de simplifyMesh do far.
// ERUPTION_LOD_FAR_MIN_AREA=0.6 volta ao comportamento antigo p/ A-B.
// Escala MAXIMA de um cartao sobrevivente na poda com compensacao de area
// (pruneCardsAreaPreserving). sqrt(area_total/area_mantida) pede ~3x para
// 107 k -> 12 k tris; o clamp segura o caso extremo (far a 400 tris pediria
// 16x e viraria meia duzia de lencois). O far usa 2x este valor.
// ERUPTION_CARD_LOD_MAX_SCALE=<f> (bancada).
static const float kCardLodMaxScale = [] {
    const char* e = std::getenv("ERUPTION_CARD_LOD_MAX_SCALE");
    return e ? std::max(1.0f, static_cast<float>(std::atof(e))) : 4.0f;
}();
static const float kFarCardMinArea = [] {
    const char* e = std::getenv("ERUPTION_LOD_FAR_MIN_AREA");
    return e ? std::strtof(e, nullptr) : 0.25f;
}();
// Idem para o LOD de SOMBRA de malha de cartao. ERUPTION_LOD_SHADOW_MIN_AREA=0.6
// volta ao antigo (sombra com a copa inteira do nivel "lo").
static const float kShadowCardMinArea = [] {
    const char* e = std::getenv("ERUPTION_LOD_SHADOW_MIN_AREA");
    return e ? std::strtof(e, nullptr) : 0.3f;
}();

template<typename Keyframe>
static std::pair<const Keyframe*, const Keyframe*> findSurroundingKeys(const std::vector<Keyframe>& keys, float currentFrame) {
    if (keys.empty()) return {nullptr, nullptr};
    if (keys.size() == 1) return {&keys[0], &keys[0]};

    if (currentFrame <= keys.front().frame) {
        return {&keys.front(), &keys.front()};
    }
    if (currentFrame >= keys.back().frame) {
        return {&keys.back(), &keys.back()};
    }

    for (size_t i = 1; i < keys.size(); i++) {
        if (currentFrame < keys[i].frame) {
            return {&keys[i - 1], &keys[i]};
        }
    }
    return {&keys.back(), &keys.back()};
}

struct NodeAnimState {
    Vec3 scale = Vec3(1.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    Vec3 position = Vec3(0.0f);
};

static NodeAnimState interpolateNode(const ModelNode& node, float currentFrame) {
    NodeAnimState state;
    state.scale = node.scale;
    state.rotation = node.rotation;
    state.position = node.position;

    if (!node.scaleKeyframes.empty()) {
        if (node.scaleKeyframes.size() >= 2) {
            auto [prev, next] = findSurroundingKeys(node.scaleKeyframes, currentFrame);
            if (prev && next && prev != next) {
                float t = (currentFrame - prev->frame) / (next->frame - prev->frame);
                state.scale = glm::mix(prev->scale, next->scale, t);
            } else if (prev) {
                state.scale = prev->scale;
            }
        } else {
            state.scale = node.scaleKeyframes[0].scale;
        }
    }

    if (!node.rotKeyframes.empty()) {
        if (node.rotKeyframes.size() >= 2) {
            auto [prev, next] = findSurroundingKeys(node.rotKeyframes, currentFrame);
            if (prev && next && prev != next) {
                float t = (currentFrame - prev->frame) / (next->frame - prev->frame);
                state.rotation = glm::slerp(prev->rotation, next->rotation, t);
            } else if (prev) {
                state.rotation = prev->rotation;
            }
        } else {
            state.rotation = node.rotKeyframes[0].rotation;
        }
    }

    if (!node.posKeyframes.empty()) {
        if (node.posKeyframes.size() >= 2) {
            auto [prev, next] = findSurroundingKeys(node.posKeyframes, currentFrame);
            if (prev && next && prev != next) {
                float t = (currentFrame - prev->frame) / (next->frame - prev->frame);
                state.position = glm::mix(prev->position, next->position, t);
            } else if (prev) {
                state.position = prev->position;
            }
        } else {
            state.position = node.posKeyframes[0].position;
        }
    }

    return state;
}

// Topologia de um asset animado: imutavel, mas era reconstruida a cada frame
// junto com 6 alocacoes de ~1900 elementos. Agora e' construida uma vez.
struct AnimTopology {
    std::vector<uint32_t> childHead, childNext, roots;
    std::vector<uint8_t> hasKeys;   // 1 = o no' tem keyframe (precisa interpolar)
    // 1 = o no' e' consultado por alguma instancia animada, OU e' ancestral de
    // um que e'. Um GLB de mapa tem ~1900 nos mas so' ~280 sao lidos: percorrer
    // a hierarquia inteira era o grosso do custo. Podar pelos ancestrais mantem
    // o resultado EXATO, porque a matriz de um no' so' depende da sua cadeia.
    std::vector<uint8_t> needed;
    bool built = false;
    bool needsBuilt = false;
};
static std::unordered_map<const ModelAsset*, AnimTopology> g_animTopo;

static void computeAnimatedRenderMatrices(const ModelAsset& asset, float currentFrame,
                                           std::vector<Mat4>& renderMatrices,
                                           const std::vector<uint32_t>* queried = nullptr) {
    const size_t n = asset.nodes.size();
    // Escreve DIRETO no cache do chamador. A versao anterior devolvia um
    // static por VALOR: malloc + memcpy + free de n*64 B (n~1900 => ~121 KB)
    // por asset animado POR FRAME, so' para mover o resultado ate' o cache
    // (varredura de hot paths 2026-09-02, achado #2).
    static std::vector<NodeAnimState> states;
    renderMatrices.assign(n, Mat4(1.0f));
    states.assign(n, NodeAnimState{});

    AnimTopology& topo = g_animTopo[&asset];
    if (!topo.built || topo.hasKeys.size() != n) {
        topo.built = true;
        topo.hasKeys.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            const auto& nd = asset.nodes[i];
            topo.hasKeys[i] = (!nd.scaleKeyframes.empty() || !nd.rotKeyframes.empty() ||
                               !nd.posKeyframes.empty()) ? 1 : 0;
        }
        topo.childHead.assign(n, UINT32_MAX);
        topo.childNext.assign(n, UINT32_MAX);
        topo.roots.clear();
        for (size_t i = n; i-- > 0; ) {
            const uint32_t p = asset.nodes[i].parentIndex;
            if (p == 0xFFFFFFFF || p == i || p >= n) {
                topo.roots.push_back(static_cast<uint32_t>(i));
            } else {
                topo.childNext[i] = topo.childHead[p];
                topo.childHead[p] = static_cast<uint32_t>(i);
            }
        }
    }
    // Mascara de nos necessarios: os consultados + todos os seus ancestrais.
    if (queried && !topo.needsBuilt) {
        topo.needsBuilt = true;
        topo.needed.assign(n, 0);
        for (uint32_t q : *queried) {
            uint32_t cur = q;
            uint32_t guard = 0;
            while (cur < n && !topo.needed[cur] && guard++ < n) {
                topo.needed[cur] = 1;
                const uint32_t par = asset.nodes[cur].parentIndex;
                if (par == 0xFFFFFFFF || par == cur || par >= n) break;
                cur = par;
            }
        }
    }
    const bool prune = topo.needsBuilt && topo.needed.size() == n;

    // interpolateNode SO' importa para no' com keyframe: o corpo abaixo usa
    // `state` unicamente quando a lista de keyframes correspondente nao e'
    // vazia (node.posKeyframes.empty() ? node.position : state.position). Num
    // GLB de mapa a esmagadora maioria dos nos e' estatica, entao interpolar
    // todos era o grosso dos ~8 ms por frame medidos em parana_field.
    for (size_t i = 0; i < n; ++i) {
        if (topo.hasKeys[i] && (!prune || topo.needed[i]))
            states[i] = interpolateNode(asset.nodes[i], currentFrame);
    }
    const std::vector<uint32_t>& childHead = topo.childHead;
    const std::vector<uint32_t>& childNext = topo.childNext;
    const std::vector<uint32_t>& roots = topo.roots;


    const uint32_t version = asset.versionMajor * 256 + asset.versionMinor;

    // Explicit stack instead of a std::function recursion: no per-call
    // indirect dispatch, and no stack-depth risk on deep node hierarchies.
    struct Item { uint32_t idx; Mat4 parent; };
    static std::vector<Item> stack;
    stack.clear();
    stack.reserve(n);
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        if (prune && !topo.needed[*it]) continue;
        stack.push_back({*it, Mat4(1.0f)});
    }

    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        const ModelNode& node = asset.nodes[item.idx];
        const auto& state = states[item.idx];

        Mat4 nodeMatrix = item.parent;

        Vec3 pos = node.posKeyframes.empty() ? node.position : state.position;
        nodeMatrix = glm::translate(nodeMatrix, pos);

        if (!node.rotKeyframes.empty()) {
            nodeMatrix = nodeMatrix * glm::mat4_cast(state.rotation);
        } else {
            nodeMatrix = nodeMatrix * glm::mat4_cast(node.rotation);
        }

        Vec3 scl = node.scaleKeyframes.empty() ? node.scale : state.scale;
        nodeMatrix = glm::scale(nodeMatrix, scl);

        Mat4 renderMatrix;
        if (version >= 0x0202) {
            renderMatrix = nodeMatrix * node.offsetMatrix;
        } else {
            renderMatrix = nodeMatrix;
            if (n > 1) {
                renderMatrix = glm::translate(renderMatrix, node.offset);
            }
            renderMatrix = renderMatrix * node.offsetMatrix;
        }
        renderMatrices[item.idx] = renderMatrix;

        for (uint32_t c = childHead[item.idx]; c != UINT32_MAX; c = childNext[c]) {
            if (prune && !topo.needed[c]) continue; // ramo sem no' consultado
            stack.push_back({c, nodeMatrix});
        }
    }

}

struct TextureAnimState {
    Vec2 translate = Vec2(0.0f);
    Vec2 scale = Vec2(1.0f);
    float rotation = 0.0f;
    bool active = false;
};

static TextureAnimState interpolateTextureAnimation(const ModelNode& node, int32_t textureId,
                                                    float currentFrame, int32_t animLen) {
    TextureAnimState result;
    result.active = false;

    if (node.animTextures.empty()) {
        return result;
    }

    int32_t cycle = glm::max(1, animLen);
    float tick = fmod(currentFrame, static_cast<float>(cycle));
    if (tick < 0.0f) tick += cycle;

    for (const auto& anim : node.animTextures) {
        if (anim.textureId != textureId || anim.keyframes.empty()) {
            continue;
        }

        const auto& keys = anim.keyframes;

        const AnimTextureKeyframe* prevKey = nullptr;
        const AnimTextureKeyframe* nextKey = nullptr;
        for (size_t i = 0; i < keys.size(); i++) {
            if (tick < keys[i].frame) {
                nextKey = &keys[i];
                prevKey = (i == 0) ? &keys.back() : &keys[i - 1];
                break;
            }
        }
        if (!nextKey) {
            prevKey = &keys.back();
            nextKey = &keys.front();
        }

        float prevTick = prevKey->frame;
        float nextTick = nextKey->frame;
        if (nextTick <= prevTick) nextTick += static_cast<float>(cycle);
        if (tick < prevTick) tick += static_cast<float>(cycle);

        float interval = 0.0f;
        if (nextTick > prevTick) {
            interval = (tick - prevTick) / (nextTick - prevTick);
        }
        float value = glm::mix(prevKey->offset, nextKey->offset, interval);

        switch (anim.type) {
        case 0: result.translate.x += value; result.active = true; break;
        case 1: result.translate.y += value; result.active = true; break;
        case 2: result.scale.x = value; result.active = true; break;
        case 3: result.scale.y = value; result.active = true; break;
        case 4: result.rotation = value; result.active = true; break;
        default: break;
        }
    }
    return result;
}

VkDeviceSize ModelMeshGPU::destroyBaseBuffers(VulkanContext* ctx) {
    VkDeviceSize freed = 0;
    if (vertexBuffer != VK_NULL_HANDLE) {
        VmaAllocationInfo vInfo;
        vmaGetAllocationInfo(ctx->allocator(), vertexAlloc, &vInfo);
        PROFILE_VRAM_FREE(ProfilerCategory::Meshes, vInfo.size);
        freed += vInfo.size;
        vmaDestroyBuffer(ctx->allocator(), vertexBuffer, vertexAlloc);
        vertexBuffer = VK_NULL_HANDLE;
    }
    if (indexBuffer != VK_NULL_HANDLE) {
        VmaAllocationInfo iInfo;
        vmaGetAllocationInfo(ctx->allocator(), indexAlloc, &iInfo);
        PROFILE_VRAM_FREE(ProfilerCategory::Meshes, iInfo.size);
        freed += iInfo.size;
        vmaDestroyBuffer(ctx->allocator(), indexBuffer, indexAlloc);
        indexBuffer = VK_NULL_HANDLE;
    }
    // Buffers do LOD geométrico. Sem isto a VMA aborta no shutdown
    // ("Some allocations were not freed before destruction of this memory
    // block") - eram alocados no load e nunca destruídos.
    if (hiVertexBuffer != VK_NULL_HANDLE) {
        VmaAllocationInfo hv;
        vmaGetAllocationInfo(ctx->allocator(), hiVertexAlloc, &hv);
        PROFILE_VRAM_FREE(ProfilerCategory::Meshes, hv.size);
        freed += hv.size;
        vmaDestroyBuffer(ctx->allocator(), hiVertexBuffer, hiVertexAlloc);
        hiVertexBuffer = VK_NULL_HANDLE;
        hiVertexAlloc = VK_NULL_HANDLE;
    }
    if (hiIndexBuffer != VK_NULL_HANDLE) {
        VmaAllocationInfo hi;
        vmaGetAllocationInfo(ctx->allocator(), hiIndexAlloc, &hi);
        PROFILE_VRAM_FREE(ProfilerCategory::Meshes, hi.size);
        freed += hi.size;
        vmaDestroyBuffer(ctx->allocator(), hiIndexBuffer, hiIndexAlloc);
        hiIndexBuffer = VK_NULL_HANDLE;
        hiIndexAlloc = VK_NULL_HANDLE;
    }
    hiIndexCount = 0;
    return freed;
}

VkDeviceSize ModelMeshGPU::destroyLodBuffers(VulkanContext* ctx) {
    VkDeviceSize freed = 0;
    if (loIndexBuffer != VK_NULL_HANDLE) {
        VmaAllocationInfo lo;
        vmaGetAllocationInfo(ctx->allocator(), loIndexAlloc, &lo);
        PROFILE_VRAM_FREE(ProfilerCategory::Meshes, lo.size);
        freed += lo.size;
        vmaDestroyBuffer(ctx->allocator(), loIndexBuffer, loIndexAlloc);
        loIndexBuffer = VK_NULL_HANDLE;
        loIndexAlloc = VK_NULL_HANDLE;
    }
    loIndexCount = 0;
    if (loVertexBuffer != VK_NULL_HANDLE) {
        VmaAllocationInfo lv;
        vmaGetAllocationInfo(ctx->allocator(), loVertexAlloc, &lv);
        PROFILE_VRAM_FREE(ProfilerCategory::Meshes, lv.size);
        freed += lv.size;
        vmaDestroyBuffer(ctx->allocator(), loVertexBuffer, loVertexAlloc);
        loVertexBuffer = VK_NULL_HANDLE;
        loVertexAlloc = VK_NULL_HANDLE;
    }
    if (farVertexBuffer != VK_NULL_HANDLE) {
        VmaAllocationInfo fv;
        vmaGetAllocationInfo(ctx->allocator(), farVertexAlloc, &fv);
        PROFILE_VRAM_FREE(ProfilerCategory::Meshes, fv.size);
        freed += fv.size;
        vmaDestroyBuffer(ctx->allocator(), farVertexBuffer, farVertexAlloc);
        farVertexBuffer = VK_NULL_HANDLE;
        farVertexAlloc = VK_NULL_HANDLE;
    }
    if (farIndexBuffer != VK_NULL_HANDLE) {
        VmaAllocationInfo fa;
        vmaGetAllocationInfo(ctx->allocator(), farIndexAlloc, &fa);
        PROFILE_VRAM_FREE(ProfilerCategory::Meshes, fa.size);
        freed += fa.size;
        vmaDestroyBuffer(ctx->allocator(), farIndexBuffer, farIndexAlloc);
        farIndexBuffer = VK_NULL_HANDLE;
        farIndexAlloc = VK_NULL_HANDLE;
    }
    farIndexCount = 0;
    if (shIndexBuffer != VK_NULL_HANDLE) {
        VmaAllocationInfo sh;
        vmaGetAllocationInfo(ctx->allocator(), shIndexAlloc, &sh);
        PROFILE_VRAM_FREE(ProfilerCategory::Meshes, sh.size);
        freed += sh.size;
        vmaDestroyBuffer(ctx->allocator(), shIndexBuffer, shIndexAlloc);
        shIndexBuffer = VK_NULL_HANDLE;
        shIndexAlloc = VK_NULL_HANDLE;
    }
    shIndexCount = 0;
    return freed;
}

VkDeviceSize ModelMeshGPU::destroyBuffers(VulkanContext* ctx) {
    return destroyBaseBuffers(ctx) + destroyLodBuffers(ctx);
}

void ModelMeshGPU::shutdown(VulkanContext* ctx) {
    // PROPRIEDADE DIVIDIDA, e e' isto que o isExternal descreve:
    //  - vertex/index/hi* vem do cache de assets (MapUpload.cpp) e sao dele;
    //    a malha externa NAO os libera, senao libera duas vezes.
    //  - lo*/far*/sh* nascem AQUI, em loadMapModels, DEPOIS de o resolver
    //    devolver a copia do cache - a copia do cache nunca os ve'. Sao do
    //    ModelRenderer mesmo quando isExternal, e tem que ser liberados
    //    sempre. Antes o `if (isExternal) return;` no topo pulava os dois
    //    grupos: 48 index buffers de LOD/sombra vivos no fechamento (VMA
    //    aborta em build de debug) e mais um lote a cada warp.
    destroyLodBuffers(ctx);
    if (!isExternal) destroyBaseBuffers(ctx);
}

bool ModelRenderer::init(VulkanContext* ctx, BindlessDescriptor* bindless) {
    m_ctx = ctx;
    m_bindless = bindless;

    VkSamplerCreateInfo samplerInfo{}; samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;

    if (vkCreateSampler(m_ctx->device(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        ERUPTION_LOG_ERROR("ModelRenderer: Failed to create sampler");
        return false;
    }

    createPipeline();

    m_initialized = (m_pipeline != VK_NULL_HANDLE);
    return m_initialized;
}

namespace {

// Quanto uma malha balanca ao vento. 0 = rigida.
//
// O caminho CONFIAVEL e' a categoria autorada em assets/data/pbr_materials.json.
// Mas mapas externos (parana_field, por exemplo) nao tem perfil: das 1015
// malhas dele, ZERO tinha categoria, entao um gate so' por categoria deixava a
// vegetacao inteira parada e o vento invisivel - que era exatamente o sintoma.
//
// Dai o fallback por nome. Ele e' heuristica declarada, nao adivinhacao
// escondida: casa os termos que as bibliotecas CC0 de fato usam nos nomes
// (Poly Haven e afins), que e' de onde vem a vegetacao destes mapas. Autorar a
// categoria continua sendo o certo e tem precedencia.
// BALANCO E' PROPRIEDADE GEOMETRICA, NAO DE MATERIAL. A estrutura verga no
// vento ou nao verga; isso nao tem relacao com "a textura parece grama?". A
// versao anterior decidia pelo classificador de PBR, que responde outra
// pergunta ("isto e' aspero? dieletrico?"), e o resultado, medido nos mapas
// legados, era invertido nas DUAS pontas:
//
//   CHAO BALANCANDO: as texturas de chao de campo sao de grama, e
//   o classificador as marca category="grass". A regra antiga
//   `category == "grass" -> 1.00` punha o TERRENO INTEIRO pra balancar. Medido:
//   19 materiais de chao em cidade-D, 42 somando com colinas-10.
//
//   FOLHA RIGIDA: o classificador marca folha e tronco como category="wood"
//   (materia vegetal seca - correto pro PBR), e a regra antiga
//   `!category.empty() -> 0.0` matava o balanco. Medido: myo-leaf1/2/3,
//   pron-leaf_1/2, j_sakura01 (cerejeira), pay-bamboo1 (bambu) - todas rigidas.
//
//   E foi isso que produziu o relato "algumas arvores balancam e outras nao":
//   na MESMA arvore de colinas-10, myo-leaf5 vinha SEM categoria, caia no nome,
//   achava "leaf" e balancava 1.0; myo-leaf1 vinha cat=wood e ficava parada.
//
// Agora o nome/pasta decide e a categoria de PBR nao tem voto. parana_demo nao
// e' afetado: seus 164 materiais vem todos sem categoria e ja' acertavam pelo
// nome (botany/branch/flower/tree/plant/weed).
float resolveSwayAmount(const std::string& texName, const std::string& category) {
    (void)category; // ver o bloco acima: responde outra pergunta.

    std::string n;
    n.reserve(texName.size());
    for (char c : texName) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto has = [&](const char* k) { return n.find(k) != std::string::npos; };

    // 0. CHAO NUNCA VERGA, por mais que a textura seja de grama. E' malha de
    //    terreno: nao ha' haste pra vergar.
    if (has("ground") || has("floor") || has("terrain")) return 0.0f;

    // 1. Folha solta e flor: a parte que mais treme.
    if (has("grass") || has("flower") || has("fern") || has("leaf") || has("ivy") ||
        has("weed")  || has("clover") || has("sakura"))
        return 1.00f;

    // 2. Arbusto e planta de porte medio. Bambu entra aqui: haste alta e fina,
    //    verga muito, e o asset legado se chama literalmente "bamboo".
    if (has("bush") || has("shrub") || has("plant") || has("botany") ||
        has("foliage") || has("vegetation") || has("palm") || has("bamboo"))
        return 0.70f;

    // 3. Arvore e galho: o tronco e' rigido, so' a copa cede. O modelo ja'
    //    escala com o quadrado da altura, entao a base fica parada sozinha.
    if (has("tree") || has("branch") || has("twig"))
        return 0.45f;

    return 0.0f;
}

} // namespace

void ModelRenderer::shutdown() {
    // Instancing: buffers mapeados + pool + layout do set 2.
    destroyInstanceBuffers(false);
    destroyShadowBuffers(false);
    if (m_instPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_ctx->device(), m_instPool, nullptr);
        m_instPool = VK_NULL_HANDLE;
    }
    if (m_instLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_ctx->device(), m_instLayout, nullptr);
        m_instLayout = VK_NULL_HANDLE;
    }
    m_instCapacity = 0;

    clear();
    if (m_tessPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_tessPipeline, nullptr);
        m_tessPipeline = VK_NULL_HANDLE;
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_ctx->device(), m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }

    for (auto& entry : m_pbrTextureCache) {
        entry.second.reset(m_ctx, m_bindless);
    }
    m_pbrTextureCache.clear();

    m_initialized = false;
}

void ModelRenderer::clear() {
    for (auto& mesh : m_meshes) {
        mesh.shutdown(m_ctx);
    }
    m_meshes.clear();
    m_instances.clear();
    m_textureSlots.clear();
    m_animatedNodes.clear();
    m_animMatrixCache.clear();
    m_textureAnimatedNodes.clear();
    m_modelMeshCache.clear();
    m_textureCache.clear();
    m_lastModels.clear();
    m_lastInstances.clear();

    // Release per-map PBR texture bindings so we don't leak images/slots across
    // map changes. They will be reloaded on demand via the texture resolver.
    for (auto& entry : m_pbrTextureCache) {
        entry.second.reset(m_ctx, m_bindless);
    }
    m_pbrTextureCache.clear();
}


// BANCADA: face descartada das pipelines de G-buffer.
//
// Todas nascem com VK_CULL_MODE_NONE - nenhum backface e' descartado, entao
// objeto solido rasteriza os dois lados. Isso e' deliberado (o acervo legado
// mistura enrolamentos e a folhagem e' de face unica) mas nunca tinha sido
// MEDIDO. ERUPTION_TEST_CULL=back|front|none faz a medida.
//
// Medido em parana_demo 25/400 (2026-09-10): back derruba invocacoes de
// fragmento de 7,50 M para 4,48 M (-40%) e o G-buffer de 5,67 para 4,80 ms,
// MAS muda 22,9% da tela e abre buraco de fundo no chao; front e' pior ainda
// (68,4%), o que prova que a maioria da malha esta' com enrolamento certo e o
// problema e' uma minoria invertida. Tentativa de descarte POR MALHA (teste de
// malha fechada + volume com sinal, casando aresta por posicao) esta'
// registrada em docs/tefra/basalto-tessellation-medido.md: e' geometricamente
// segura mas rende so' 1,1%, porque a tela e' dominada por chao e folhagem, e
// AINDA mexe na imagem - superficie fechada cujo material usa descarte por
// alfa mostra o proprio lado de dentro. Nao vale a complexidade. Fica so' o
// instrumento.
static VkCullModeFlags gbufferCullMode() {
    static const VkCullModeFlags kMode = [] {
        const char* e = std::getenv("ERUPTION_TEST_CULL");
        if (!e) return VK_CULL_MODE_NONE;
        const std::string v(e);
        if (v == "back")  return VK_CULL_MODE_BACK_BIT;
        if (v == "front") return VK_CULL_MODE_FRONT_BIT;
        return VK_CULL_MODE_NONE;
    }();
    return kMode;
}

void ModelRenderer::createPipeline() {
    auto vertCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/model.vert.spv");
    auto fragCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/model.frag.spv"); 
    if (vertCode.empty() || fragCode.empty()) {
        ERUPTION_LOG_ERROR("ModelRenderer: Failed to load model shaders");
        return;
    }

    VkShaderModule vertModule, fragModule;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertCode.size() * sizeof(uint32_t);
    smInfo.pCode = vertCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &vertModule);

    smInfo.codeSize = fragCode.size() * sizeof(uint32_t);
    smInfo.pCode = fragCode.data();
    vkCreateShaderModule(m_ctx->device(), &smInfo, nullptr, &fragModule);

    std::vector<VkPipelineShaderStageCreateInfo> stages(2);
    stages[0] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1] = {};
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(TerrainVertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attribs(18);
    attribs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TerrainVertex, position)};
    attribs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TerrainVertex, texCoord)};
    attribs[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TerrainVertex, normal)};
    attribs[3] = {3, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, texIndex)};
    attribs[4] = {4, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, matId)};
    attribs[5] = {5, 0, VK_FORMAT_R32_UINT, offsetof(TerrainVertex, color)};
    attribs[6] = {6, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, pbrIndex)};
    attribs[7] = {7, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, normalIndex)};
    attribs[8] = {8, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendTexIndex)};
    attribs[9] = {9, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendPbrIndex)};
    attribs[10] = {10, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendNormalIndex)};
    attribs[11] = {11, 0, VK_FORMAT_R32_SFLOAT, offsetof(TerrainVertex, blendWeight)};
    attribs[12] = {12, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendMaskIndex)};
    attribs[13] = {13, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TerrainVertex, blendMaskUV)};
    attribs[14] = {14, 0, VK_FORMAT_R32_SFLOAT, offsetof(TerrainVertex, emissiveStrength)};
    attribs[15] = {15, 0, VK_FORMAT_R32G32_UINT, offsetof(TerrainVertex, splatTex01)};
    attribs[16] = {16, 0, VK_FORMAT_R32G32_UINT, offsetof(TerrainVertex, splatTex45)};
    attribs[17] = {17, 0, VK_FORMAT_R16_UINT, offsetof(TerrainVertex, blendMaskIndex2)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribs.size());
    vertexInput.pVertexAttributeDescriptions = attribs.data();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                             VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                             VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    pcRange.offset = 0;
    // Push struct: viewProj (mat4) + model (mat4) + alpha + metallicScale + roughnessScale + pad (vec2)
    //              + uvTranslateRot (vec4) + uvScale (vec4)
    pcRange.size = sizeof(Mat4) * 2 + sizeof(Vec4) * 2 + sizeof(Vec4);

    VkDescriptorSetLayout bindlessLayout = m_bindless->layout();

    // Set 2: SSBO com os dados por instancia (ver model.vert). Criado uma vez.
    if (m_instLayout == VK_NULL_HANDLE) {
        VkDescriptorSetLayoutBinding ib{};
        ib.binding = 0;
        ib.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ib.descriptorCount = 1;
        ib.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ici.bindingCount = 1;
        ici.pBindings = &ib;
        vkCreateDescriptorSetLayout(m_ctx->device(), &ici, nullptr, &m_instLayout);
    }
    VkDescriptorSetLayout layouts[3] = { bindlessLayout, m_frameUboLayout, m_instLayout };

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = (m_frameUboLayout != VK_NULL_HANDLE) ? 3u : 1u;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_pipelineLayout);

    std::vector<VkPipelineColorBlendAttachmentState> blends(5);
    for (auto& b : blends) {
        b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        b.blendEnable = VK_FALSE;
    }

    m_pipeline = PipelineBuilder()
        .setShaderStages(stages)
        .setVertexInput(vertexInput)
        .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setViewport(0, 0, 1280, 720)
        .setScissor(0, 0, 1280, 720)
        .setPolygonMode(wireframeMode() ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL)
        .setCullMode(gbufferCullMode(), VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthState(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setBlendState(blends)
        .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .setLayout(m_pipelineLayout)
        .setColorAttachmentFormats(GBuffer::colorAttachmentFormats())
        .setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .build(m_ctx->device());

    // PIPELINE DE TESSELACAO (banda perto). Mesmo layout, mesmos buffers,
    // mesma malha - so' muda a topologia (patch de 3 pontos) e os dois
    // estagios a mais. Se o driver nao tiver a feature, ou os SPIR-V nao
    // existirem, fica NULL e o render usa o pipeline normal: nada quebra.
    if (m_ctx->tessellationSupported()) {
        auto tescCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/model.tesc.spv");
        auto teseCode = ShaderCompiler::loadSPIRV("shaders/gbuffer/model.tese.spv");
        if (!tescCode.empty() && !teseCode.empty()) {
            VkShaderModule tescModule = VK_NULL_HANDLE, teseModule = VK_NULL_HANDLE;
            VkShaderModuleCreateInfo tsm{};
            tsm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            tsm.codeSize = tescCode.size() * sizeof(uint32_t);
            tsm.pCode = tescCode.data();
            vkCreateShaderModule(m_ctx->device(), &tsm, nullptr, &tescModule);
            tsm.codeSize = teseCode.size() * sizeof(uint32_t);
            tsm.pCode = teseCode.data();
            vkCreateShaderModule(m_ctx->device(), &tsm, nullptr, &teseModule);
            if (tescModule != VK_NULL_HANDLE && teseModule != VK_NULL_HANDLE) {
                VkPipelineShaderStageCreateInfo tescStage{};
                tescStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                tescStage.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
                tescStage.module = tescModule;
                tescStage.pName = "main";
                VkPipelineShaderStageCreateInfo teseStage = tescStage;
                teseStage.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
                teseStage.module = teseModule;

                std::vector<VkPipelineShaderStageCreateInfo> tessStages = { stages[0], tescStage, teseStage, stages[1] };
                m_tessPipeline = PipelineBuilder()
                    .setShaderStages(tessStages)
                    .setVertexInput(vertexInput)
                    .setPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)
                    .setPatchControlPoints(3)
                    .setViewport(0, 0, 1280, 720)
                    .setScissor(0, 0, 1280, 720)
                    .setPolygonMode(wireframeMode() ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL)
                    .setCullMode(gbufferCullMode(), VK_FRONT_FACE_COUNTER_CLOCKWISE)
                    .setDepthState(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
                    .setBlendState(blends)
                    .setDynamicState({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
                    .setLayout(m_pipelineLayout)
                    .setColorAttachmentFormats(GBuffer::colorAttachmentFormats())
                    .setDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
                    .build(m_ctx->device());
                vkDestroyShaderModule(m_ctx->device(), tescModule, nullptr);
                vkDestroyShaderModule(m_ctx->device(), teseModule, nullptr);
                ERUPTION_LOG_WARN("[TESS] pipeline de tesselacao %s",
                                  m_tessPipeline != VK_NULL_HANDLE ? "criado" : "FALHOU");
            }
        }
    }

    vkDestroyShaderModule(m_ctx->device(), vertModule, nullptr);
    vkDestroyShaderModule(m_ctx->device(), fragModule, nullptr);
}

uint32_t ModelRenderer::resolveTexture(const std::string& path) {
    if (path.empty()) return 0;

    std::string sanitizedPath = path;
    std::replace(sanitizedPath.begin(), sanitizedPath.end(), '/', '\\');

    // Remove leading backslash if any
    if (!sanitizedPath.empty() && sanitizedPath[0] == '\\') sanitizedPath = sanitizedPath.substr(1);

    auto it = m_textureCache.find(sanitizedPath);
    if (it != m_textureCache.end()) {
        return it->second;
    }

    uint32_t slot = 0;
    if (m_textureResolver) {
        slot = m_textureResolver(sanitizedPath);
    } else {
        ERUPTION_LOG_WARN("ModelRenderer: no texture resolver set, cannot load '%s'", sanitizedPath.c_str());
    }

    m_textureCache[sanitizedPath] = slot;
    return slot;
}

PbrTextureSlots ModelRenderer::resolvePbrTextures(const std::string& path) {
    if (path.empty()) return PbrTextureSlots{};

    std::string sanitizedPath = path;
    std::replace(sanitizedPath.begin(), sanitizedPath.end(), '/', '\\');
    if (!sanitizedPath.empty() && sanitizedPath[0] == '\\') sanitizedPath = sanitizedPath.substr(1);

    auto it = m_pbrTextureCache.find(sanitizedPath);
    if (it != m_pbrTextureCache.end()) {
        return it->second;
    }

    PbrTextureSlots slots = loadPbrTexturesForAlbedo(m_ctx, m_bindless, m_sampler, sanitizedPath);
    static const bool kTrace = std::getenv("ERUPTION_TEST_PBR_TRACE") != nullptr;
    if (kTrace) {
        ERUPTION_LOG_WARN("[PBRSLOT] '%s' -> valid=%d mrahw=%u normal=%u",
                          sanitizedPath.c_str(), slots.valid() ? 1 : 0,
                          slots.mrahw.index, slots.normal.index);
    }
    m_pbrTextureCache[sanitizedPath] = slots;
    return slots;
}

struct MeshUploadData {
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t texSlot = 0;
    Vec3 aabbMin;
    Vec3 aabbMax;
};

void ModelRenderer::loadMapModels(const std::vector<ModelAsset>& models,
                                   const std::vector<ModelInstanceDesc>& instances,
                                   float centerX, float centerZ) {
    clear();

    if (models.empty() || instances.empty()) return;

    m_lastModels = models;
    m_lastInstances = instances;

    std::unordered_map<std::string, uint32_t> modelMap;
    for (uint32_t i = 0; i < m_lastModels.size(); ++i) {
        std::string normalizedPath = m_lastModels[i].filePath;
        std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
        modelMap[normalizedPath] = i;
    }

    ERUPTION_LOG_INFO("ModelRenderer: loadMapModels with %zu refs", instances.size());

    for (const auto& ref : instances) {
        std::string modelPath = ref.modelPath;
        std::replace(modelPath.begin(), modelPath.end(), '\\', '/');

        auto it = modelMap.find(modelPath);
        if (it == modelMap.end()) it = modelMap.find("data/" + modelPath);
        if (it == modelMap.end() && modelPath.find("data/") == 0) it = modelMap.find(modelPath.substr(5));
        if (it == modelMap.end()) continue;

        const ModelAsset& asset = m_lastModels[it->second];
        if (asset.nodes.empty()) continue;

        uint32_t version = asset.versionMajor * 256 + asset.versionMinor;

        // Ensure model meshes are uploaded to GPU
        if (m_modelMeshCache.find(asset.filePath) == m_modelMeshCache.end()) {
            std::vector<uint32_t> meshIndices(asset.nodes.size(), 0xFFFFFFFF);
            size_t dbgReidxOk = 0, dbgReidxFail = 0, dbgVertsBefore = 0, dbgVertsAfter = 0;

            std::vector<uint32_t> assetTexIndexToBindlessSlot(asset.textures.size(), 0);
            for (size_t t = 0; t < asset.textures.size(); ++t) {
                assetTexIndexToBindlessSlot[t] = resolveTexture(asset.textures[t]);
            }

            for (size_t n = 0; n < asset.nodes.size(); ++n) {
                const auto& node = asset.nodes[n];
                if (node.vertices.empty() || node.indices.empty()) continue;

                std::vector<TerrainVertex> vertices;
                vertices.reserve(node.vertices.size());
                
                Vec3 aabbMin(FLT_MAX);
                Vec3 aabbMax(-FLT_MAX);
                std::string firstTexName; // used for the PBR material profile lookup
                // NOME DO MATERIAL, nao o da imagem embutida. O GLB traz imagens
                // com nome sintetico ("#-00_0000.png") - o proprio parser
                // guarda o nome do MATERIAL em node.pbrTextureNames justamente
                // por isso. O perfil PBR e o balanco ao vento eram resolvidos
                // pelo nome sintetico, entao NENHUM dos dois casava: no
                // parana_field 135 das 164 malhas ficavam com sway 0 e a
                // vegetacao nao balancava, por mais forte que fosse o vento
                // (medido: no tornado a forca chega a 1,14 no shader e nada se
                // mexia). As texturas de verdade do mapa chamam
                // "pine_sapling_medium_twig", que a heuristica reconhece.
                std::string firstPbrName;
                if (!node.pbrTextureNames.empty()) firstPbrName = node.pbrTextureNames[0];

                // Crossfade targets resolve to the same handful of slots for the
                // whole node; cache per local texture index so the per-vertex
                // path is a plain lookup instead of a string hash every vertex.
                struct BlendResolved { uint32_t tex = 0, pbrIdx = 0, normalIdx = 0; bool done = false; };
                std::vector<BlendResolved> blendCache(node.textureNames.size());

                // World-space splat-mask texture resolve cache (new, optional).
                struct MaskResolved { uint32_t tex = 0; bool done = false; };
                std::vector<MaskResolved> maskCache(node.textureNames.size());
                // Mesmo cache pras 4 camadas de splat: resolver por nome a
                // cada vertice seria caro e o conjunto e' minusculo.
                std::vector<MaskResolved> splatCache(node.textureNames.size());

                // Primary texture / PBR resolve is identical for every vertex
                // sharing a faceTexIdx. Doing the string sanitize + hash lookups
                // per vertex (millions on a baked map GLB) was ~15s of the load;
                // cache the result per faceTexIdx and the vertex body becomes a
                // vector index.
                struct FaceResolved {
                    uint32_t texSlot = 0, pbrIdx = 0, normalIdx = 0;
                    std::string name;
                    bool done = false;
                };
                size_t faceCacheSize = node.textureNames.size();
                if (!node.textureIds.empty()) faceCacheSize = std::max(faceCacheSize, node.textureIds.size());
                faceCacheSize = std::max(faceCacheSize, asset.textures.size());
                if (faceCacheSize == 0) faceCacheSize = 1;
                std::vector<FaceResolved> faceCache(faceCacheSize);

                for (size_t i = 0; i < node.vertices.size(); ++i) {
                    uint16_t faceTexIdx = (i < node.perVertexTexIds.size()) ? node.perVertexTexIds[i] : 0;

                    FaceResolved& fr = faceCache[faceTexIdx % faceCacheSize];
                    if (!fr.done) {
                        fr.done = true;
                        uint32_t texSlot = 0;
                        std::string texName;
                        std::string pbrName;
                        if (!node.textureNames.empty()) {
                            // v2.3+: per-node names
                            texName = node.textureNames[faceTexIdx % node.textureNames.size()];
                            texSlot = resolveTexture(texName);
                            if (!node.pbrTextureNames.empty()) {
                                pbrName = node.pbrTextureNames[faceTexIdx % node.pbrTextureNames.size()];
                            }
                        } else if (!node.textureIds.empty()) {
                            // v1.x-2.2: map local ID to global list
                            uint32_t globalIdx = node.textureIds[faceTexIdx % node.textureIds.size()];
                            texSlot = (globalIdx < assetTexIndexToBindlessSlot.size()) ? assetTexIndexToBindlessSlot[globalIdx] : 0;
                            if (globalIdx < asset.textures.size()) texName = asset.textures[globalIdx];
                        } else {
                            // Fallback: use global index directly
                            texSlot = (faceTexIdx < assetTexIndexToBindlessSlot.size()) ? assetTexIndexToBindlessSlot[faceTexIdx] : 0;
                            if (faceTexIdx < asset.textures.size()) texName = asset.textures[faceTexIdx];
                        }
                        PbrTextureSlots pbrSlots;
                        if (!pbrName.empty()) {
                            pbrSlots = resolvePbrTextures(pbrName);
                        }
                        // Fallback para o nome da TEXTURA: o nome do material
                        // acha o PBR cozido em disco, mas quando ele nao existe
                        // a sintese precisa dos pixels, e textura embutida no
                        // GLB so e' encontravel pelo nome da imagem (mangled
                        // "#___"). Sem este fallback a ponte do cidade-D (e todo
                        // material sem PBR cozido em mapa com textura embutida)
                        // ficava sem normal map nenhum - ou seja, ZERO relevo.
                        if (!pbrSlots.valid() && !texName.empty() && texName != pbrName) {
                            pbrSlots = resolvePbrTextures(texName);
                        }
                        fr.texSlot = texSlot;
                        fr.pbrIdx = pbrSlots.mrahw.index;
                        fr.normalIdx = pbrSlots.normal.index;
                        fr.name = std::move(texName);
                    }
                    uint32_t texSlot = fr.texSlot;
                    PbrTextureSlots pbrSlots;
                    pbrSlots.mrahw.index = fr.pbrIdx;
                    pbrSlots.normal.index = fr.normalIdx;

                    if (firstTexName.empty() && !fr.name.empty()) {
                        firstTexName = fr.name;
                    }

                    // GLB-authored texture crossfade: resolve the blend-toward
                    // texture the same way as the primary. Inert when the node
                    // carries no _BLEND_WEIGHT data.
                    uint32_t blendTexSlot = 0, blendPbrIdx = 0, blendNormalIdx = 0;
                    float blendWeight = (i < node.perVertexBlendWeights.size())
                                            ? node.perVertexBlendWeights[i] : 0.0f;
                    if (blendWeight > 0.0f && i < node.perVertexBlendTexIds.size() &&
                        !node.textureNames.empty()) {
                        uint16_t bLocal = static_cast<uint16_t>(
                            node.perVertexBlendTexIds[i] % node.textureNames.size());
                        BlendResolved& br = blendCache[bLocal];
                        if (!br.done) {
                            br.done = true;
                            br.tex = resolveTexture(node.textureNames[bLocal]);
                            std::string bPbrName = (bLocal < node.pbrTextureNames.size())
                                ? node.pbrTextureNames[bLocal] : node.textureNames[bLocal];
                            if (bPbrName.empty()) bPbrName = node.textureNames[bLocal];
                            if (!bPbrName.empty()) {
                                PbrTextureSlots s = resolvePbrTextures(bPbrName);
                                br.pbrIdx = s.mrahw.index;
                                br.normalIdx = s.normal.index;
                            }
                        }
                        blendTexSlot = br.tex;
                        blendPbrIdx = br.pbrIdx;
                        blendNormalIdx = br.normalIdx;
                    }

                    // World-space splat mask: resolve the same way as the primary
                    // texture. Inert when the node carries no _BLEND_MASK_UV data.
                    uint32_t maskTexSlot = 0;
                    Vec2 maskUV(0.0f);
                    if (i < node.perVertexBlendMaskTexIds.size() &&
                        i < node.perVertexBlendMaskUV.size() &&
                        node.perVertexBlendMaskTexIds[i] != 0 &&
                        !node.textureNames.empty()) {
                        uint16_t mLocal = static_cast<uint16_t>(
                            node.perVertexBlendMaskTexIds[i] % node.textureNames.size());
                        MaskResolved& mr = maskCache[mLocal];
                        if (!mr.done) {
                            mr.done = true;
                            mr.tex = resolveTexture(node.textureNames[mLocal]);
                        }
                        maskTexSlot = mr.tex;
                        maskUV = node.perVertexBlendMaskUV[i];
                    }

                    // Segunda mascara de splat: mesma UV da primeira (e' so'
                    // posicao de mundo), resolve por nome do mesmo jeito.
                    uint32_t maskTexSlot2 = 0;
                    if (i < node.perVertexBlendMaskTexIds2.size() &&
                        node.perVertexBlendMaskTexIds2[i] != 0 &&
                        !node.textureNames.empty()) {
                        uint16_t mLocal2 = static_cast<uint16_t>(
                            node.perVertexBlendMaskTexIds2[i] % node.textureNames.size());
                        MaskResolved& mr2 = maskCache[mLocal2];
                        if (!mr2.done) {
                            mr2.done = true;
                            mr2.tex = resolveTexture(node.textureNames[mLocal2]);
                        }
                        maskTexSlot2 = mr2.tex;
                    }

                    TerrainVertex v{};
                    v.position = node.vertices[i].position;
                    v.texCoord = node.vertices[i].texCoord;
                    v.normal = node.vertices[i].normal;
                    v.texIndex = texSlot;
                    v.matId = node.vertices[i].matId;
                    v.color = node.vertices[i].color;
                    v.pbrIndex = pbrSlots.mrahw.index;
                    v.normalIndex = pbrSlots.normal.index;
                    v.blendTexIndex = blendTexSlot;
                    v.blendPbrIndex = blendPbrIdx;
                    v.blendNormalIndex = blendNormalIdx;
                    v.blendWeight = (blendTexSlot != 0) ? blendWeight : 0.0f;
                    v.blendMaskIndex = maskTexSlot;
                    v.blendMaskUV = maskUV;
                    v.blendMaskIndex2 = maskTexSlot2;
                    v.emissiveStrength = (i < node.perVertexEmissive.size()) ? node.perVertexEmissive[i] : 0.0f;
                    // Splat: ate' 8 slots de 16 bits empacotados em 4 uint32. O
                    // slot local vira slot GLOBAL de bindless do mesmo jeito
                    // que o texSlot da camada base logo acima.
                    if (!node.textureNames.empty()) {
                        uint32_t sg[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                        for (int sIdx = 0; sIdx < 8; ++sIdx) {
                            const auto& arr = node.perVertexSplatTexIds[sIdx];
                            if (i >= arr.size() || arr[i] == 0) continue;
                            uint16_t sLocal = static_cast<uint16_t>(
                                arr[i] % node.textureNames.size());
                            MaskResolved& sr = splatCache[sLocal];
                            if (!sr.done) {
                                sr.done = true;
                                sr.tex = resolveTexture(node.textureNames[sLocal]);
                            }
                            sg[sIdx] = sr.tex;
                        }
                        v.splatTex01 = (sg[0] & 0xFFFFu) | ((sg[1] & 0xFFFFu) << 16);
                        v.splatTex23 = (sg[2] & 0xFFFFu) | ((sg[3] & 0xFFFFu) << 16);
                        v.splatTex45 = (sg[4] & 0xFFFFu) | ((sg[5] & 0xFFFFu) << 16);
                        v.splatTex67 = (sg[6] & 0xFFFFu) | ((sg[7] & 0xFFFFu) << 16);
                    }
                    vertices.push_back(v);

                    aabbMin = glm::min(aabbMin, v.position);
                    aabbMax = glm::max(aabbMax, v.position);
                }

                // Reindexa: a malha vem DESINDEXADA do parser (um vértice por
                // canto de triângulo), o que faz o vertex shader rodar ~2,3x
                // mais que o necessário e zera o reuso do cache pós-transform.
                // Medido: o passe GBuffer custa o mesmo (3,41 / 3,44 / 3,55 ms)
                // para 0,31 / 1,31 / 2,30 Mpx - não escala com resolução, logo
                // é vértice, não fragmento. A solda é por igualdade de BYTES do
                // vértice inteiro, então a imagem não muda.
                // ERUPTION_NO_REINDEX=1 volta ao comportamento antigo (A/B).
                static const bool kNoReindex = std::getenv("ERUPTION_NO_REINDEX") != nullptr;
                const size_t reVtxCountBefore = vertices.size();
                const std::vector<uint32_t>* useIndices = &node.indices;
                std::vector<uint32_t> reIdx;
                std::vector<unsigned char> reVtx;
                if (!kNoReindex && !vertices.empty() &&
                    reindexMesh(vertices.data(), vertices.size(), sizeof(TerrainVertex),
                                node.indices, reVtx, reIdx)) {
                    vertices.assign(
                        reinterpret_cast<const TerrainVertex*>(reVtx.data()),
                        reinterpret_cast<const TerrainVertex*>(reVtx.data()) +
                            reVtx.size() / sizeof(TerrainVertex));
                    useIndices = &reIdx;
                    ++dbgReidxOk; dbgVertsBefore += reVtxCountBefore; dbgVertsAfter += vertices.size();
                } else if (!kNoReindex) {
                    ++dbgReidxFail; dbgVertsBefore += vertices.size(); dbgVertsAfter += vertices.size();
                }

                ModelMeshGPU mesh;
                // Prefere o nome do MATERIAL; cai no da imagem se nao houver.
                const std::string& nomeMaterial = !firstPbrName.empty() ? firstPbrName : firstTexName;
                const auto& profile = getPbrProfile(nomeMaterial);
                // REORDENACAO ESPACIAL DO CHAO. Ordena os triangulos por celula
                // de kGroundCellSize e guarda o intervalo de indices de cada
                // uma: e' o que permite desenhar SO' o pedaco perto da camera
                // com tesselacao. Custa uma ordenacao no load e zero por frame.
                std::vector<uint32_t> groundIdx;
                std::vector<ModelMeshGPU::GroundCell> groundCells;
                // MESMO criterio do mesh.isGround (ver mais abaixo): categoria,
                // nome coreano de "chao" OU vertices com splat - no
                // parana_field, gerado por ferramenta, e' o splat que marca.
                bool splatGround = false;
                for (const auto& v : vertices) {
                    if (v.splatTex01 != 0u || v.splatTex23 != 0u) { splatGround = true; break; }
                }
                const bool isGroundMesh = splatGround || profile.category == "ground" ||
                                          nomeMaterial.find("\xEB\xB0\x94\xEB\x8B\xA5") != std::string::npos;
                if (isGroundMesh && useIndices->size() >= 3 * 64) {
                    constexpr float kGroundCellSize = 96.0f;
                    struct Cell { std::vector<uint32_t> idx; float minX, minZ, maxX, maxZ; };
                    std::unordered_map<uint64_t, Cell> cells;
                    for (size_t t = 0; t + 2 < useIndices->size(); t += 3) {
                        const Vec3& p0 = vertices[(*useIndices)[t]].position;
                        const Vec3& p1 = vertices[(*useIndices)[t + 1]].position;
                        const Vec3& p2 = vertices[(*useIndices)[t + 2]].position;
                        const float cx = (p0.x + p1.x + p2.x) / 3.0f;
                        const float cz = (p0.z + p1.z + p2.z) / 3.0f;
                        const int64_t gx = static_cast<int64_t>(std::floor(cx / kGroundCellSize));
                        const int64_t gz = static_cast<int64_t>(std::floor(cz / kGroundCellSize));
                        const uint64_t key = (static_cast<uint64_t>(gx + 32768) << 20) ^
                                              static_cast<uint64_t>(gz + 32768);
                        Cell& c = cells[key];
                        if (c.idx.empty()) {
                            c.minX = c.maxX = p0.x; c.minZ = c.maxZ = p0.z;
                        }
                        for (int k = 0; k < 3; ++k) {
                            const Vec3& pp = vertices[(*useIndices)[t + k]].position;
                            c.minX = std::min(c.minX, pp.x); c.maxX = std::max(c.maxX, pp.x);
                            c.minZ = std::min(c.minZ, pp.z); c.maxZ = std::max(c.maxZ, pp.z);
                            c.idx.push_back((*useIndices)[t + k]);
                        }
                    }
                    groundIdx.reserve(useIndices->size());
                    groundCells.reserve(cells.size());
                    for (auto& kv : cells) {
                        ModelMeshGPU::GroundCell gc{kv.second.minX, kv.second.minZ,
                                                    kv.second.maxX, kv.second.maxZ,
                                                    static_cast<uint32_t>(groundIdx.size()),
                                                    static_cast<uint32_t>(kv.second.idx.size())};
                        groundIdx.insert(groundIdx.end(), kv.second.idx.begin(), kv.second.idx.end());
                        groundCells.push_back(gc);
                    }
                    // Em ORDEM DE BUFFER: o desenho funde intervalos vizinhos
                    // com o mesmo veredito, e so' isso derruba 1.764 draws para
                    // algumas dezenas.
                    std::sort(groundCells.begin(), groundCells.end(),
                              [](const ModelMeshGPU::GroundCell& a, const ModelMeshGPU::GroundCell& b) {
                                  return a.first < b.first;
                              });
                    if (groundIdx.size() == useIndices->size()) {
                        useIndices = &groundIdx;
                        ERUPTION_LOG_WARN("[TESS] chao particionado em %zu celulas de %.0f u (%zu indices)",
                                          groundCells.size(), kGroundCellSize, groundIdx.size());
                    } else {
                        groundCells.clear();
                    }
                }
                std::string meshKey = asset.filePath + "#" + std::to_string(n);
                if (m_meshResolver) {
                    mesh = m_meshResolver(meshKey, vertices, *useIndices);
                    mesh.aabbMin = aabbMin;
                    mesh.aabbMax = aabbMax;
                } else {
                    VkDeviceSize vertexSize = vertices.size() * sizeof(TerrainVertex);
                    VkDeviceSize indexSize = useIndices->size() * sizeof(uint32_t);
                    
                    m_ctx->createBuffer(vertexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY, mesh.vertexBuffer, mesh.vertexAlloc);
                    m_ctx->createBuffer(indexSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY, mesh.indexBuffer, mesh.indexAlloc);
                    // Espelho dos PROFILE_VRAM_FREE de destroyBuffers - sem o
                    // ALLOC o contador de Meshes so' descia (pedidos.md #5).
                    PROFILE_VRAM_ALLOC(ProfilerCategory::Meshes, vertexSize + indexSize);
                    
                    VkBuffer stagingBuffer;
                    VmaAllocation stagingAlloc;
                    m_ctx->createBuffer(vertexSize + indexSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, stagingBuffer, stagingAlloc);
                    void* mapped;
                    vmaMapMemory(m_ctx->allocator(), stagingAlloc, &mapped);
                    std::memcpy(mapped, vertices.data(), vertexSize);
                    std::memcpy((uint8_t*)mapped + vertexSize, useIndices->data(), indexSize);
                    vmaUnmapMemory(m_ctx->allocator(), stagingAlloc);

                    m_ctx->immediateSubmit([&](VkCommandBuffer cmd) {
                        VkBufferCopy cv{0, 0, vertexSize};
                        vkCmdCopyBuffer(cmd, stagingBuffer, mesh.vertexBuffer, 1, &cv);
                        VkBufferCopy ci{vertexSize, 0, indexSize};
                        vkCmdCopyBuffer(cmd, stagingBuffer, mesh.indexBuffer, 1, &ci);
                    });
                    vmaDestroyBuffer(m_ctx->allocator(), stagingBuffer, stagingAlloc);

                    mesh.indexCount = static_cast<uint32_t>(useIndices->size());
                }
                mesh.groundCells = std::move(groundCells);

                // MATERIAL E VENTO DEPOIS DO RESOLVER, nunca antes. O
                // m_meshResolver (cache de geometria) retorna um ModelMeshGPU
                // NOVO e a atribuicao `mesh = m_meshResolver(...)` substituia a
                // struct inteira - tudo que fosse escrito acima era descartado
                // em silencio. Perdiam-se swayAmount, metallicScale e
                // roughnessScale de TODA malha resolvida pelo cache.
                // Sintoma que levou ate' aqui (autor 2026-09-02: "tem que
                // implementar o vento, nao to vendo nada se movendo"): o vento
                // chegava ao vertex shader (medido: 0,082 em tempo limpo, 0,76
                // em tempestade) e o balanco por malha era calculado certo
                // (medido: 29 de 164 malhas com sway > 0), mas o valor que
                // chegava ao SSBO era 0,00 em todas - a vegetacao ficava
                // congelada. Confirmado por sonda: forcar sway em todas as
                // malhas nao mudava um pixel, enquanto deslocar sem ler o sway
                // movia 98,65% da tela.
                mesh.metallicScale = profile.metallic;
                mesh.roughnessScale = profile.roughness;
                mesh.swayAmount = resolveSwayAmount(nomeMaterial, profile.category);
                // FATOR DE DESLOCAMENTO POR MATERIAL (tesselacao da banda
                // perto). Chao, terra e neve tem relevo de verdade e levam a
                // amplitude cheia. Pedra e madeira levam menos: a malha de
                // arquitetura tem triangulo GRANDE e chapado, e deslocar por
                // altura de textura ali estica o triangulo em espeto - visto
                // em cidade-D com amplitude 3. Telhado, metal e agua ficam em
                // zero: superficie dura, lisa ou com shader proprio.
                {
                    const std::string& cat = profile.category;
                    mesh.dispScale = (cat == "ground" || cat == "dirt" || cat == "snow") ? 1.0f
                                   : (cat == "grass")                                   ? 0.6f
                                   : (cat == "stone")                                   ? 0.45f
                                   : (cat == "wood")                                    ? 0.30f
                                   : (cat == "roof" || cat == "metal" ||
                                      cat == "water" || cat == "vegetation")            ? 0.0f
                                                                                        : 0.35f;
                }
                // ERUPTION_TEST_SWAY_ALL=<v>: forca o balanco em TODAS as malhas
                // (diagnostico: separa "o vento nao chega" de "a heuristica de
                // nome nao classificou nada como vegetacao").
                static const float kSwayAll = [] {
                    const char* e = std::getenv("ERUPTION_TEST_SWAY_ALL");
                    return e ? static_cast<float>(std::atof(e)) : 0.0f;
                }();
                if (kSwayAll > 0.0f) mesh.swayAmount = kSwayAll;
                if (std::getenv("ERUPTION_TEST_SWAY_STATS"))
                    ERUPTION_LOG_WARN("[SWAY] %s cat=%s sway=%.2f", nomeMaterial.c_str(),
                                      profile.category.c_str(), mesh.swayAmount);
                // CHAO (G38): terreno que veio como modelo. Tres pistas, qualquer
                // uma basta: categoria "ground" do perfil PBR, "ground" no nome
                // do material, ou vertices com splat (terreno procedural GLB).
                {
                    bool splat = false;
                    for (const auto& v : vertices) {
                        if (v.splatTex01 != 0u || v.splatTex23 != 0u) { splat = true; break; }
                    }
                    mesh.isGround = splat || profile.category == "ground" ||
                                    nomeMaterial.find("ground") != std::string::npos;
                }

                // CASTER SOLIDO? So' se TODA textura referenciada pela malha
                // for opaca. Basta um vertice apontar para textura com alfa
                // para a malha inteira continuar no caminho com teste - errar
                // para o lado do caro nao quebra imagem, o contrario faz cerca
                // virar retangulo solido no shadow map.
                {
                    bool allOpaque = m_bindless != nullptr && !vertices.empty();
                    if (allOpaque) {
                        for (const auto& v : vertices) {
                            if (!m_bindless->slotOpaque(v.texIndex)) { allOpaque = false; break; }
                        }
                    }
                    mesh.shadowOpaque = allOpaque;
                    if (std::getenv("ERUPTION_TEST_SHADOW_OPAQUE_CENSUS")) {
                        static int nTot = 0, nOpaque = 0;
                        ++nTot;
                        if (allOpaque) ++nOpaque;
                        ERUPTION_LOG_WARN("[SOMBRA-OPACA] malhas=%d solidas=%d", nTot, nOpaque);
                    }
                }

                // LOD grosseiro por distância. Um index buffer simplificado
                // sobre o MESMO vertex buffer: a instância longe desenha menos
                // triângulo sem uma segunda cópia da malha na VRAM.
                //
                // Motivo medido no parana_field: 713 instâncias visíveis
                // submetiam 3.902.788 triângulos por frame. Compartilhar
                // geometria entre instâncias resolveu memória, mas não reduz o
                // que é rasterizado - só LOD reduz, e é o que decide se o mapa
                // roda na 930M.
                if (m_meshLodRatio > 0.0f && meshLodAvailable() &&
                    useIndices->size() >= kMeshLodMinIndices &&
                    useIndices->size() >= size_t(m_meshLodMinTris) * 3 && !vertices.empty()) {
                    MeshLodResult lod = simplifyMesh(&vertices[0].position.x,
                                                     vertices.size(), sizeof(TerrainVertex),
                                                     *useIndices, m_meshLodRatio, m_meshLodError,
                                                     m_meshLodMaxTris,
                                                     offsetof(TerrainVertex, texCoord),
                                                     offsetof(TerrainVertex, texIndex),
                                                     mesh.swayAmount > 0.0f);
                    // FOLHAGEM: poda de cartoes com COMPENSACAO DE AREA (Cook,
                    // Halstead, Planck, Ryu 2007 - ver MeshLod.hpp). O caminho
                    // de cartao do simplifyMesh so' remove e para no piso de
                    // 60% de area, entao o teto de triangulos nao era honrado:
                    // medido no parana_demo a pitch 30, as malhas 50/51 (107 k
                    // e 99 k tris) ficavam em 45 k/41 k no "lo" e, com 155+141
                    // instancias, eram 9,75 M dos 17,4 M tris do frame - para
                    // 2,7 M fragmentos (~5 triangulos por fragmento). Aqui os
                    // cartoes que ficam CRESCEM em torno do proprio centroide e a
                    // cobertura da copa se conserva, entao lo e far cabem no
                    // teto de verdade. Como os vertices mudam, cada nivel tem
                    // vertex buffer proprio (lo/farVertexBuffer).
                    // ERUPTION_TEST_NO_CARD_PRUNE=1: volta ao caminho antigo (A/B).
                    static const bool kNoCardPrune = std::getenv("ERUPTION_TEST_NO_CARD_PRUNE") != nullptr;
                    bool cardLodDone = false;
                    if (lod.cardMesh && !kNoCardPrune && mesh.swayAmount > 0.0f) {
                        auto uploadBuf = [&](const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
                                             VkBuffer& buf, VmaAllocation& alloc) {
                            m_ctx->createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                                                VMA_MEMORY_USAGE_GPU_ONLY, buf, alloc);
                            PROFILE_VRAM_ALLOC(ProfilerCategory::Meshes, size);
                            VkBuffer stg; VmaAllocation stgAlloc;
                            m_ctx->createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                VMA_MEMORY_USAGE_CPU_ONLY, stg, stgAlloc);
                            void* mp = nullptr;
                            vmaMapMemory(m_ctx->allocator(), stgAlloc, &mp);
                            std::memcpy(mp, data, static_cast<size_t>(size));
                            vmaUnmapMemory(m_ctx->allocator(), stgAlloc);
                            m_ctx->immediateSubmit([&](VkCommandBuffer c) {
                                VkBufferCopy cp{0, 0, size};
                                vkCmdCopyBuffer(c, stg, buf, 1, &cp);
                            });
                            vmaDestroyBuffer(m_ctx->allocator(), stg, stgAlloc);
                        };
                        const size_t triCount = useIndices->size() / 3;
                        size_t loTarget = static_cast<size_t>(double(triCount) * m_meshLodRatio);
                        if (m_meshLodMaxTris > 0) loTarget = std::min<size_t>(loTarget, m_meshLodMaxTris);
                        loTarget = std::max<size_t>(loTarget, 3);
                        CardPruneResult pr = pruneCardsAreaPreserving(
                            vertices.data(), vertices.size(), sizeof(TerrainVertex),
                            offsetof(TerrainVertex, position), *useIndices,
                            static_cast<uint32_t>(loTarget),
                            offsetof(TerrainVertex, texCoord), offsetof(TerrainVertex, texIndex),
                            kCardLodMaxScale);
                        if (pr.valid) {
                            uploadBuf(pr.vertices.data(), pr.vertices.size(),
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, mesh.loVertexBuffer, mesh.loVertexAlloc);
                            uploadBuf(pr.indices.data(), pr.indices.size() * sizeof(uint32_t),
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mesh.loIndexBuffer, mesh.loIndexAlloc);
                            mesh.loIndexCount = static_cast<uint32_t>(pr.indices.size());
                            // Distancia de troca: mesma convencao do dropCards
                            // (area removida x 0,25 x extensao), mas o crescimento
                            // devolve a cobertura, entao so' a fracao que a escala
                            // NAO compensou conta como erro.
                            const float extent = glm::length(mesh.aabbMax - mesh.aabbMin);
                            const float uncompensated = 1.0f - std::min(1.0f, pr.areaKeptRaw * pr.scale * pr.scale);
                            const float absErr = std::max(((1.0f - pr.areaKeptRaw) * 0.08f + uncompensated * 0.25f) * extent, 1e-3f);
                            mesh.loSwitchDistance = std::max(absErr * m_meshLodDistanceFactor, m_meshLodMinDistance);
                            cardLodDone = true;
                            if (m_meshLodFarMaxTris > 0 && mesh.loIndexCount / 3 > m_meshLodFarMaxTris * 2) {
                                CardPruneResult fr = pruneCardsAreaPreserving(
                                    vertices.data(), vertices.size(), sizeof(TerrainVertex),
                                    offsetof(TerrainVertex, position), *useIndices,
                                    m_meshLodFarMaxTris,
                                    offsetof(TerrainVertex, texCoord), offsetof(TerrainVertex, texIndex),
                                    kCardLodMaxScale * 2.0f);
                                if (fr.valid && fr.indices.size() < mesh.loIndexCount) {
                                    uploadBuf(fr.vertices.data(), fr.vertices.size(),
                                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, mesh.farVertexBuffer, mesh.farVertexAlloc);
                                    uploadBuf(fr.indices.data(), fr.indices.size() * sizeof(uint32_t),
                                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mesh.farIndexBuffer, mesh.farIndexAlloc);
                                    mesh.farIndexCount = static_cast<uint32_t>(fr.indices.size());
                                    mesh.farSwitchDistance = mesh.loSwitchDistance * m_meshLodFarDistanceMul;
                                }
                            }
                            static const bool kCardPruneDbg = std::getenv("ERUPTION_DEBUG_LODCARDS") != nullptr;
                            if (kCardPruneDbg)
                                ERUPTION_LOG_WARN("[CARDPRUNE] %s: %zu -> lo %u tris (escala %.2f, area bruta %.3f, ilhas %u/%u) far %u | troca lo=%.0f far=%.0f",
                                                  nomeMaterial.c_str(), triCount, mesh.loIndexCount / 3,
                                                  pr.scale, pr.areaKeptRaw, pr.islandsKept, pr.islandsTotal,
                                                  mesh.farIndexCount / 3, mesh.loSwitchDistance, mesh.farSwitchDistance);
                        }
                    }
                    if (!cardLodDone && lod.valid) {
                        const VkDeviceSize loSize = lod.indices.size() * sizeof(uint32_t);
                        m_ctx->createBuffer(loSize,
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                            VMA_MEMORY_USAGE_GPU_ONLY, mesh.loIndexBuffer, mesh.loIndexAlloc);
                        PROFILE_VRAM_ALLOC(ProfilerCategory::Meshes, loSize);
                        VkBuffer stg; VmaAllocation stgAlloc;
                        m_ctx->createBuffer(loSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                            VMA_MEMORY_USAGE_CPU_ONLY, stg, stgAlloc);
                        void* mp = nullptr;
                        vmaMapMemory(m_ctx->allocator(), stgAlloc, &mp);
                        std::memcpy(mp, lod.indices.data(), loSize);
                        vmaUnmapMemory(m_ctx->allocator(), stgAlloc);
                        m_ctx->immediateSubmit([&](VkCommandBuffer c) {
                            VkBufferCopy cp{0, 0, loSize};
                            vkCmdCopyBuffer(c, stg, mesh.loIndexBuffer, 1, &cp);
                        });
                        vmaDestroyBuffer(m_ctx->allocator(), stg, stgAlloc);
                        mesh.loIndexCount = static_cast<uint32_t>(lod.indices.size());
                        // Distância de troca vem do ERRO GEOMÉTRICO medido pela
                        // simplificação, não do raio da malha. Escalar pelo raio
                        // era exatamente o contrário do desejado: dava a maior
                        // distância de troca justamente às malhas maiores - as
                        // que carregam o triângulo todo - e elas nunca trocavam
                        // (medido: troca variando de 10 a 998 unidades, com a
                        // câmera a 400). Com o erro, uma malha cuja simplificação
                        // desloca pouco troca cedo, e o erro já escala com o
                        // tamanho da malha por construção.
                        // meshopt reporta o erro RELATIVO à extensão da malha,
                        // não em unidades de mundo: converter antes de usar.
                        // Sem isso as distâncias de troca saíam entre 0,1 e 6,6
                        // unidades, ou seja, o LOD entrava colado na câmera.
                        const float extent = glm::length(mesh.aabbMax - mesh.aabbMin);
                        const float absErr = std::max(lod.error * extent, 1e-3f);
                        // Troca quando o erro projeta abaixo do limiar em pixels:
                        // distância = erro_mundo * (altura_viewport / 2tan(fov/2))
                        // / limiar_px. O fator do preset é essa constante.
                        // Piso de distancia: nada troca de LOD perto da camera,
                        // por menor que seja o erro geometrico da malha. Sem
                        // isto, objeto fino (poste) troca a ~30 unidades
                        // enquanto predio troca a ~180, e o poste pipoca.
                        mesh.loSwitchDistance = std::max(absErr * m_meshLodDistanceFactor,
                                                         m_meshLodMinDistance);

                        // LOD DISTANTE: so' faz sentido se for bem menor que o
                        // "lo". Simplificacao "sloppy" (sem preservar topologia)
                        // - a esta distancia a silhueta grossa e' tudo que a
                        // tela resolve. Entra a um multiplo da troca do "lo".
                        if (m_meshLodFarMaxTris > 0 &&
                            mesh.loIndexCount / 3 > m_meshLodFarMaxTris * 2) {
                            MeshLodResult fl = simplifyMesh(&vertices[0].position.x,
                                                            vertices.size(), sizeof(TerrainVertex),
                                                            *useIndices,
                                                            std::max(m_meshLodRatio * 0.25f, 0.02f),
                                                            1.0f /* erro livre: o TETO manda */, m_meshLodFarMaxTris,
                                                            offsetof(TerrainVertex, texCoord),
                                                            offsetof(TerrainVertex, texIndex),
                                                            mesh.swayAmount > 0.0f,
                                                            // PISO DE AREA do LOD DISTANTE em malha de cartao. O
                                                            // piso padrao (0,6: a copa so' afina) e' certo para o
                                                            // nivel "lo", que ainda e' visto de perto - mas ele
                                                            // tambem vetava o distante: com 60% da area obrigatoria
                                                            // o dropCards nunca chega a 400 triangulos, o far sai
                                                            // igual ao lo e NUNCA e' usado. Medido no parana_demo
                                                            // (2026-09-05, pitch 15, zoom 1000): duas malhas de
                                                            // ~100k tri x ~340 instancias ficavam em 45k CADA a
                                                            // qualquer distancia = 30 M dos 42 M tri do frame.
                                                            // Alem de 3x a troca do lo a copa e' pequena na tela;
                                                            // afinar mais e' o que o olho nao ve e a GPU sente.
                                                            kFarCardMinArea);
                            if (fl.valid && fl.indices.size() < mesh.loIndexCount) {
                                const VkDeviceSize faSize = fl.indices.size() * sizeof(uint32_t);
                                m_ctx->createBuffer(faSize,
                                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                                    VMA_MEMORY_USAGE_GPU_ONLY, mesh.farIndexBuffer, mesh.farIndexAlloc);
                        PROFILE_VRAM_ALLOC(ProfilerCategory::Meshes, faSize);
                                VkBuffer st3; VmaAllocation st3a;
                                m_ctx->createBuffer(faSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                    VMA_MEMORY_USAGE_CPU_ONLY, st3, st3a);
                                void* mp3 = nullptr;
                                vmaMapMemory(m_ctx->allocator(), st3a, &mp3);
                                std::memcpy(mp3, fl.indices.data(), faSize);
                                vmaUnmapMemory(m_ctx->allocator(), st3a);
                                m_ctx->immediateSubmit([&](VkCommandBuffer c) {
                                    VkBufferCopy cp{0, 0, faSize};
                                    vkCmdCopyBuffer(c, st3, mesh.farIndexBuffer, 1, &cp);
                                });
                                vmaDestroyBuffer(m_ctx->allocator(), st3, st3a);
                                mesh.farIndexCount = static_cast<uint32_t>(fl.indices.size());
                                mesh.farSwitchDistance = mesh.loSwitchDistance * m_meshLodFarDistanceMul;
                            }
                        }
                    }

                    // LOD exclusivo da sombra: mais agressivo, porque a
                    // silhueta no shadow map nao resolve o detalhe (RMS 0,000
                    // medido). So' vale a pena se for menor que o LOD de tela.
                    if (m_shadowLodMaxTris > 0) {
                        MeshLodResult sh = simplifyMesh(&vertices[0].position.x,
                                                        vertices.size(), sizeof(TerrainVertex),
                                                        *useIndices, m_meshLodRatio,
                                                        m_meshLodError * 4.0f, m_shadowLodMaxTris,
                                                        offsetof(TerrainVertex, texCoord),
                                                        offsetof(TerrainVertex, texIndex),
                                                        mesh.swayAmount > 0.0f,
                                                        // Mesmo problema do far (ver acima): com o piso
                                                        // de 60% de area o LOD de SOMBRA de uma copa saia
                                                        // igual ao "lo" (45k tri) e era rejeitado logo
                                                        // abaixo - a sombra desenhava 45k tri por arvore,
                                                        // 680 instancias, e o passe custava 5-8 ms no
                                                        // parana_demo (2026-09-05). A sombra e' filtrada
                                                        // (PCSS 16 taps): copa com 30% dos cartoes ainda
                                                        // projeta sombra continua, so' mais clara.
                                                        kShadowCardMinArea);
                        if (sh.valid && (mesh.loIndexCount == 0 || sh.indices.size() < mesh.loIndexCount)) {
                            const VkDeviceSize shSize = sh.indices.size() * sizeof(uint32_t);
                            m_ctx->createBuffer(shSize,
                                                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                                VMA_MEMORY_USAGE_GPU_ONLY, mesh.shIndexBuffer, mesh.shIndexAlloc);
                        PROFILE_VRAM_ALLOC(ProfilerCategory::Meshes, shSize);
                            VkBuffer st2; VmaAllocation st2a;
                            m_ctx->createBuffer(shSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                VMA_MEMORY_USAGE_CPU_ONLY, st2, st2a);
                            void* mp2 = nullptr;
                            vmaMapMemory(m_ctx->allocator(), st2a, &mp2);
                            std::memcpy(mp2, sh.indices.data(), shSize);
                            vmaUnmapMemory(m_ctx->allocator(), st2a);
                            m_ctx->immediateSubmit([&](VkCommandBuffer c) {
                                VkBufferCopy cp{0, 0, shSize};
                                vkCmdCopyBuffer(c, st2, mesh.shIndexBuffer, 1, &cp);
                            });
                            vmaDestroyBuffer(m_ctx->allocator(), st2, st2a);
                            mesh.shIndexCount = static_cast<uint32_t>(sh.indices.size());
                        }
                    }
                }

                m_meshes.push_back(mesh);
                meshIndices[n] = static_cast<uint32_t>(m_meshes.size() - 1);
            }
            // Nós que compartilham geometria (geometryShareIndex >= 0) não
            // subiram malha nenhuma: apontam para o ModelMeshGPU do dono. Passe
            // separado porque a ordem dos nós não garante que o dono venha
            // antes. Sem isso, uma árvore instanciada 883 vezes viraria 883
            // cópias da mesma malha na VRAM.
            for (size_t n = 0; n < asset.nodes.size(); ++n) {
                if (meshIndices[n] != 0xFFFFFFFF) continue;
                int32_t src = asset.nodes[n].geometryShareIndex;
                if (src >= 0 && src < static_cast<int32_t>(asset.nodes.size())) {
                    meshIndices[n] = meshIndices[src];
                }
            }
            if (std::getenv("ERUPTION_DEBUG_DRAWSTATS")) {
                size_t withLod = 0; float minSw = 1e9f, maxSw = 0.0f;
                size_t tri = 0, triLo = 0;
                for (const auto& mm : m_meshes) {
                    tri += mm.indexCount / 3;
                    if (mm.loIndexCount > 0) {
                        ++withLod; triLo += mm.loIndexCount / 3;
                        minSw = std::min(minSw, mm.loSwitchDistance);
                        maxSw = std::max(maxSw, mm.loSwitchDistance);
                    } else { triLo += mm.indexCount / 3; }
                }
                ERUPTION_LOG_WARN("[REIDX] soldadas=%zu falhou=%zu vertices %zu -> %zu (%.2fx) "
                                  "| vertex buffer %.1f -> %.1f MB (economia %.1f MB, %zu B/vertice)",
                                  dbgReidxOk, dbgReidxFail, dbgVertsBefore, dbgVertsAfter,
                                  dbgVertsAfter ? double(dbgVertsBefore)/double(dbgVertsAfter) : 1.0,
                                  dbgVertsBefore * sizeof(TerrainVertex) / 1e6,
                                  dbgVertsAfter * sizeof(TerrainVertex) / 1e6,
                                  (dbgVertsBefore - dbgVertsAfter) * sizeof(TerrainVertex) / 1e6,
                                  sizeof(TerrainVertex));
                ERUPTION_LOG_WARN("[LODGEN] ratio=%.2f meshopt=%d malhas=%zu com_lod=%zu "
                                  "tris_full=%zu tris_lod=%zu troca=[%.1f..%.1f]",
                                  m_meshLodRatio, (int)meshLodAvailable(), m_meshes.size(),
                                  withLod, tri, triLo,
                                  withLod ? minSw : 0.0f, maxSw);
            }
            m_modelMeshCache[asset.filePath] = meshIndices;
        }

        // Create instances for each node
        const auto& meshIndices = m_modelMeshCache[asset.filePath];
        
        // source model v1 (Y-down): source world Y-up position must be flipped to Y-down model space.
        // source model v2 (native Y-up): keep source world Y-up position as-is.
        Vec3 pos(ref.position.x, (version < 0x202) ? -ref.position.y : ref.position.y, ref.position.z);
        Mat4 transMat = glm::translate(Mat4(1.0f), pos + Vec3(centerX, 0.0f, centerZ));
        Mat4 rot = Mat4(1.0f);
        rot = glm::rotate(rot, glm::radians(-ref.rotation.z), Vec3(0.0f, 0.0f, 1.0f));
        rot = glm::rotate(rot, glm::radians(-ref.rotation.x), Vec3(1.0f, 0.0f, 0.0f));
        rot = glm::rotate(rot, glm::radians(ref.rotation.y), Vec3(0.0f, 1.0f, 0.0f));
        Vec3 scale = ref.scale;
        if (version < 0x202) {
            scale.y = -scale.y; // Global flip to convert Y-down to Y-up
        }
        Mat4 instanceMatrix = transMat * rot * glm::scale(Mat4(1.0f), scale);

        Vec3 mainBBoxCenter = (asset.boxMin + asset.boxMax) * 0.5f;
        // source model v1 / source Y-down sources: bottom is MaxY.
        // source model v2 / native Y-up sources (e.g. exported GLB): asset is already authored
        // with ground at y=0, so do not apply a vertical shift.
        float groundY = (version < 0x202) ? asset.boxMax.y : 0.0f;

        bool boxValid = asset.boxMin.x <= asset.boxMax.x && asset.boxMin.y <= asset.boxMax.y && asset.boxMin.z <= asset.boxMax.z;

        Mat4 mainOffset = (boxValid && !asset.skipMainOffset)
            ? glm::translate(Mat4(1.0f), Vec3(-mainBBoxCenter.x, -groundY, -mainBBoxCenter.z))
            : Mat4(1.0f);

        for (size_t n = 0; n < asset.nodes.size(); ++n) {
            uint32_t meshIdx = meshIndices[n];
            if (meshIdx == 0xFFFFFFFF) continue;

            const auto& node = asset.nodes[n];

            ModelInstance inst;
            inst.meshIndex = meshIdx;
            inst.transform = instanceMatrix * mainOffset * node.renderMatrix;
            inst.name = node.name;
            inst.assetPath = asset.filePath;
            inst.nodeIndex = static_cast<uint32_t>(n);

            // Compute world AABB
            Vec3 wMin(FLT_MAX), wMax(-FLT_MAX);
            Vec3 localMin = m_meshes[meshIdx].aabbMin;
            Vec3 localMax = m_meshes[meshIdx].aabbMax;
            for (int c = 0; c < 8; c++) {
                Vec3 localCorner(
                    (c & 1) ? localMax.x : localMin.x,
                    (c & 2) ? localMax.y : localMin.y,
                    (c & 4) ? localMax.z : localMin.z
                );
                Vec3 worldCorner = Vec3(inst.transform * Vec4(localCorner, 1.0f));
                wMin = glm::min(wMin, worldCorner);
                wMax = glm::max(wMax, worldCorner);
            }
            inst.worldAabbMin = wMin;
            inst.worldAabbMax = wMax;
            inst.worldCenter = (wMin + wMax) * 0.5f;

            // Estimate world-space bounding radius from local AABB + scale
            Vec3 localSize = m_meshes[meshIdx].aabbMax - m_meshes[meshIdx].aabbMin;
            Vec3 worldSize = Vec3(
                localSize.x * std::abs(scale.x),
                localSize.y * std::abs(scale.y),
                localSize.z * std::abs(scale.z)
            );
            inst.boundingRadius = glm::length(worldSize) * 0.5f;
            m_instances.push_back(inst);

            // Detect animated nodes
            bool isAnimated = node.scaleKeyframes.size() >= 2 || node.rotKeyframes.size() >= 2 || node.posKeyframes.size() >= 2;
            if (isAnimated) {
                AnimatedNodeData anim;
                anim.modelAssetIndex = it->second;
                anim.nodeIndex = static_cast<uint32_t>(n);
                anim.instanceMatrix = instanceMatrix;
                anim.mainOffset = mainOffset;
                anim.instanceIndex = static_cast<uint32_t>(m_instances.size() - 1);
                m_animatedNodes.push_back(std::move(anim));
            }

            // Detect texture-animated nodes (lava, flowing water surfaces, etc.)
            if (!node.animTextures.empty()) {
                TextureAnimatedNodeData texAnim;
                texAnim.modelAssetIndex = it->second;
                texAnim.nodeIndex = static_cast<uint32_t>(n);
                texAnim.instanceIndex = static_cast<uint32_t>(m_instances.size() - 1);
                m_textureAnimatedNodes.push_back(std::move(texAnim));
                m_instances.back().hasTextureAnimation = true;
            }
        }
    }

    m_bindless->flushUpdates();
    rebuildHotData();
    ERUPTION_LOG_INFO("ModelRenderer: loaded %zu instances (%zu unique meshes, %zu animated nodes, %zu texture-animated)",
                    m_instances.size(), m_meshes.size(), m_animatedNodes.size(), m_textureAnimatedNodes.size());
}

void ModelRenderer::recreatePipeline() {
    if (m_tessPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_tessPipeline, nullptr);
        m_tessPipeline = VK_NULL_HANDLE;
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_ctx->device(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device(), m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    createPipeline();
    m_initialized = (m_pipeline != VK_NULL_HANDLE);
}

void ModelRenderer::setFrameUboSet(VkDescriptorSetLayout layout, VkDescriptorSet set) {
    if (m_frameUboLayout == layout && m_frameUboSet == set) return;
    m_frameUboLayout = layout;
    m_frameUboSet = set;
    recreatePipeline();
}

void ModelRenderer::destroyInstanceBuffers(bool deferred) {
    for (uint32_t f = 0; f < kInstFrames; ++f) {
        if (m_instBuffers[f] == VK_NULL_HANDLE) continue;
        VkBuffer buf = m_instBuffers[f];
        VmaAllocation alloc = m_instAllocs[f];
        VmaAllocator allocator = m_ctx->allocator();
        auto destroy = [buf, alloc, allocator]() {
            vmaUnmapMemory(allocator, alloc);
            vmaDestroyBuffer(allocator, buf, alloc);
        };
        // Em crescimento de capacidade, frames em voo ainda LEEM o buffer velho:
        // a destruicao espera o frame correspondente terminar.
        if (deferred) m_ctx->deferFrameCleanup(destroy); else destroy();
        m_instBuffers[f] = VK_NULL_HANDLE;
        m_instAllocs[f] = nullptr;
        m_instMapped[f] = nullptr;
    }
}

bool ModelRenderer::ensureInstPool() {
    if (m_instPool != VK_NULL_HANDLE) return true;
    if (m_instLayout == VK_NULL_HANDLE) return false; // criado no createPipeline
    // 3 sets do passe principal + 3 da sombra, todos com o mesmo layout.
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kInstFrames * 2};
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = kInstFrames * 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(m_ctx->device(), &pci, nullptr, &m_instPool) != VK_SUCCESS)
        return false;
    VkDescriptorSetLayout lay[kInstFrames] = { m_instLayout, m_instLayout, m_instLayout };
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_instPool;
    ai.descriptorSetCount = kInstFrames;
    ai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(m_ctx->device(), &ai, m_instSets) != VK_SUCCESS) return false;
    if (vkAllocateDescriptorSets(m_ctx->device(), &ai, m_shadowSets) != VK_SUCCESS) return false;
    return true;
}

void ModelRenderer::destroyShadowBuffers(bool deferred) {
    for (uint32_t f = 0; f < kInstFrames; ++f) {
        if (m_shadowBuffers[f] == VK_NULL_HANDLE) continue;
        VkBuffer buf = m_shadowBuffers[f];
        VmaAllocation alloc = m_shadowAllocs[f];
        VmaAllocator allocator = m_ctx->allocator();
        auto destroy = [buf, alloc, allocator]() {
            vmaUnmapMemory(allocator, alloc);
            vmaDestroyBuffer(allocator, buf, alloc);
        };
        if (deferred) m_ctx->deferFrameCleanup(destroy); else destroy();
        m_shadowBuffers[f] = VK_NULL_HANDLE;
        m_shadowAllocs[f] = nullptr;
        m_shadowMapped[f] = nullptr;
    }
}

bool ModelRenderer::ensureShadowCapacity() {
    // Margem 8x: o buffer acumula TODAS as chamadas de renderShadow do frame
    // (cascatas + passes extras). Cresce so' quando o mapa cresce, e o swap de
    // mapa espera a GPU - nunca recria com frames em voo lendo.
    const uint32_t want = static_cast<uint32_t>(m_instances.size()) * 8u;
    if (m_shadowBuffers[0] != VK_NULL_HANDLE && want <= m_shadowCapacity) return true;
    if (!ensureInstPool()) return false;
    destroyShadowBuffers(true);
    const uint32_t cap = std::max(want, 4096u);
    const VkDeviceSize bytes = VkDeviceSize(cap) * sizeof(Mat4);
    for (uint32_t f = 0; f < kInstFrames; ++f) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        if (vmaCreateBuffer(m_ctx->allocator(), &bci, &aci,
                            &m_shadowBuffers[f], &m_shadowAllocs[f], nullptr) != VK_SUCCESS) {
            destroyShadowBuffers(false);
            return false;
        }
        if (vmaMapMemory(m_ctx->allocator(), m_shadowAllocs[f], &m_shadowMapped[f]) != VK_SUCCESS) {
            destroyShadowBuffers(false);
            return false;
        }
        VkDescriptorBufferInfo bi{ m_shadowBuffers[f], 0, bytes };
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m_shadowSets[f];
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
    }
    m_shadowCapacity = cap;
    return true;
}

bool ModelRenderer::ensureInstanceCapacity(uint32_t count) {
    if (m_instBuffers[0] != VK_NULL_HANDLE && count <= m_instCapacity) return true;
    // Margem 4x: varias chamadas de render() por frame (cena + minimapa)
    // empilham no mesmo buffer via m_instCursor.
    uint32_t cap = std::max({count * 4u, m_instCapacity * 2u, 4096u});

    destroyInstanceBuffers(true);

    if (!ensureInstPool()) return false;

    const VkDeviceSize bytes = VkDeviceSize(cap) * sizeof(GpuModelInstance);
    for (uint32_t f = 0; f < kInstFrames; ++f) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        if (vmaCreateBuffer(m_ctx->allocator(), &bci, &aci,
                            &m_instBuffers[f], &m_instAllocs[f], nullptr) != VK_SUCCESS) {
            ERUPTION_LOG_ERROR("Instancing: falhou criar buffer de %u instancias", cap);
            destroyInstanceBuffers(false);
            return false;
        }
        if (vmaMapMemory(m_ctx->allocator(), m_instAllocs[f], &m_instMapped[f]) != VK_SUCCESS) {
            destroyInstanceBuffers(false);
            return false;
        }
        VkDescriptorBufferInfo bi{ m_instBuffers[f], 0, bytes };
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m_instSets[f];
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
    }
    m_instCapacity = cap;
    ERUPTION_LOG_INFO("Instancing: %u slots x %u frames (%.1f KB cada)",
                      cap, kInstFrames, bytes / 1024.0f);
    return true;
}

void ModelRenderer::render(VkCommandBuffer cmd,
                           const Mat4& viewProj, const Frustum& frustum, const Vec3& cameraPos) {
    PROFILE_CPU_SCOPE(ProfilerCategory::Culling);
    if (!m_initialized || m_instances.empty()) return;
    ++m_lodFrame;

    // SIMD/SoA frustum cull; fills m_visibleInstanceIndices.
    m_lastCameraPos = cameraPos; // ANTES do cull: o descarte por tela usa a camera deste frame
    cullInstances(frustum);

    // ---- INSTANCING (SIMT) ----
    // O laco antigo fazia, POR instancia visivel: preencher 168 bytes de push,
    // vkCmdPushConstants, selecao de LOD, e um vkCmdDrawIndexed com
    // instanceCount=1. Em parana_field eram 1940 draws e ~3 ms de CPU so' de
    // submissao (mais o espelho disso no passe de sombra). Agora os dados por
    // instancia vao num SSBO (set 2) escrito uma vez por frame, e instancias
    // consecutivas com a mesma (malha, LOD, alpha) saem num UNICO draw
    // instanciado - a GPU le u_inst[gl_InstanceIndex] em paralelo.
    const uint32_t visN = static_cast<uint32_t>(m_visibleInstanceIndices.size());
    if (visN == 0) {
        m_lastDrawnTriangles = 0;
        m_lastDrawCalls = 0;
        return;
    }
    // Runs longos exigem a lista agrupada por malha. O counting sort ja existia
    // (era opt-in por ERUPTION_DRAW_SORT); para instancing ele e' requisito.
    // Estatistica so' do passe principal (minimapa passa pixelsPerUnit = 0).
    // MOVIDO para antes da reserva de capacidade (era depois): o pre-passe de
    // crossfade abaixo precisa de m_lodOfInstance, que e' preenchido aqui.
    sortVisibleByMesh(m_visibleInstanceIndices, true, m_pixelsPerUnit > 0.0f);

    // CROSSFADE DE LOD (cheio <-> reduzido): pre-passe SO' DE CONTAGEM, sem
    // escrever SSBO ainda - precisa saber quantos slots extras reservar antes
    // de pedir capacidade. Ver o comentario grande no laco principal abaixo
    // para o design completo (dithered cross-fade, SOTA tipo Unity/Unreal/
    // Cesium: https://cesium.com/blog/2022/10/20/smoother-lod-transitions-in-cesium-for-unreal/).
    // No wireframe o crossfade nao faz sentido (o dither abriria buracos na
    // malha) e o nivel ja' e' fixo no mais grosseiro.
    static const bool kLodFadeOff =
        std::getenv("ERUPTION_NO_LOD_FADE") != nullptr || wireframeMode();
    // A BANDA E' EXATAMENTE O INTERVALO DE HISTERESE [d*kLoLodHysteresis, d].
    // Nao pode ser uma janela arbitraria em torno de d: a primeira versao usava
    // +-6% e isso deixava um DEGRAU DURO na borda de baixo - uma instancia que
    // a histerese mantem em "reduzido" ate' 0,85d desenhava 100% reduzido
    // abaixo de 0,94d e saltava pra ~2% reduzido + 98% cheio logo acima. Varrer
    // o pitch no zoom 0 arrasta muitas instancias por essa borda ao mesmo
    // tempo: e' o "jitter esquisito" que o autor viu (2026-09-03).
    // Com a banda casada com a histerese, os dois extremos fecham sozinhos:
    // abaixo de 0,85d a histerese ja' garante lod=1 (t=0, cheio inteiro) e
    // acima de d garante lod=2 (t=1, reduzido inteiro) - continuo nos dois
    // sentidos, sem depender de qual estado a histerese escolheu.
    // QUANTIZACAO do fade em degraus (nao continuo por instancia): alpha e' a
    // chave que agrupa runs (`alpha == runAlpha`), entao um valor unico por
    // instancia (cada uma na sua distancia exata) fragmenta o desenho em 1
    // draw por instancia - medido em parana_field, 23 instancias na banda
    // simultaneamente viraram +50 draws (48->98). Com 8 degraus, instancias
    // vizinhas na mesma faixa de distancia caem no MESMO alpha e voltam a
    // agrupar; o dither ja' randomiza espacialmente, entao 8 degraus por
    // transicao (tipicamente espalhados por varios frames de movimento) nao
    // e' perceptivel como "stepping" - e' o mesmo principio de quantizar LOD
    // por faixas de distancia em vez de continuo.
    constexpr float kLodFadeSteps = 8.0f;
    uint32_t companionCount = 0;
    if (!kLodFadeOff && !m_forceBaseLod) {
        for (uint32_t idx : m_visibleInstanceIndices) {
            const ModelInstanceHot& hot = m_instanceHot[idx];
            if (hot.meshIndex >= m_meshes.size()) continue;
            const auto& mesh = m_meshes[hot.meshIndex];
            const uint8_t lod = (idx < m_lodOfInstance.size()) ? m_lodOfInstance[idx] : 1;
            const LodFadePair pair = lodFadePairFor(mesh, lodSwitchFor(hot, mesh),
                                                    lodDistanceFor(idx, hot));
            if (!pair.active) continue;
            if (lod != pair.nearLevel && lod != pair.farLevel) continue;
            ++companionCount;
        }
    }

    if (!ensureInstanceCapacity(visN + companionCount)) {
        m_lastDrawnTriangles = 0;
        m_lastDrawCalls = 0;
        return;
    }
    // Cursor de append: cena e minimapa escrevem trechos distintos do mesmo
    // buffer no mesmo frame. Reseta quando o frame vira.
    const uint32_t curFrame = m_ctx->currentFrame();
    if (curFrame != m_instLastFrame) {
        m_instLastFrame = curFrame;
        m_instCursor = 0;
    }
    if (m_instCursor + visN + companionCount > m_instCapacity) {
        // Nao truncar silenciosamente: melhor pular este passe e avisar do que
        // desenhar com matriz de outro passe.
        static bool warned = false;
        if (!warned) {
            warned = true;
            ERUPTION_LOG_ERROR("Instancing: cursor estourou (%u+%u+%u > %u) - passe pulado",
                               m_instCursor, visN, companionCount, m_instCapacity);
        }
        m_lastDrawnTriangles = 0;
        m_lastDrawCalls = 0;
        return;
    }
    const uint32_t instBase = m_instCursor;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    VkDescriptorSet bindlessSet = m_bindless->set();
    const uint32_t instFrame = m_ctx->currentFrame() % kInstFrames;
    if (m_frameUboSet != VK_NULL_HANDLE) {
        VkDescriptorSet sets[3] = { bindlessSet, m_frameUboSet, m_instSets[instFrame] };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                0, 3, sets, 0, nullptr);
    } else {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                0, 1, &bindlessSet, 0, nullptr);
    }

    if (m_hiLodActive.size() != m_instances.size()) m_hiLodActive.assign(m_instances.size(), 0);
    if (m_loLodActive.size() != m_instances.size()) m_loLodActive.assign(m_instances.size(), 0);

    static const bool magentaDebug = std::getenv("ERUPTION_TEST_MAGENTA_DEBUG") != nullptr;
    const bool highlight = (m_highlightedInstanceIndex != -1);

    // Push por RUN (nao mais por instancia): vp e flags de frame + escalas de
    // material da malha. Os campos por instancia continuam no struct para o
    // layout nao mudar, mas o vertex shader nao os le mais.
    struct {
        Mat4 vp;
        Mat4 model;
        float alpha;
        float metallicScale;
        float roughnessScale;
        float __pad;
        Vec4 uvTranslateRot;
        Vec4 uvScale;
    } push;
    push.vp = viewProj;
    push.model = Mat4(1.0f);
    push.__pad = magentaDebug ? 1.0f : 0.0f;
    push.uvTranslateRot = Vec4(0.0f);
    push.uvScale = Vec4(1.0f, 1.0f, 0.0f, 0.0f);

    auto* dst = static_cast<GpuModelInstance*>(m_instMapped[instFrame]) + instBase;

    VkBuffer boundVb = VK_NULL_HANDLE;
    VkBuffer boundIb = VK_NULL_HANDLE;
    uint32_t dbgDraws = 0, dbgIdx = 0;
    // Gateado: era malloc+memset de m_meshes.size() pares POR PASSE (cena +
    // minimapa = 2x/frame) so' para debug DESLIGADO. (Varredura 2026-09-02.)
    static const bool kDbgStatsAlloc = std::getenv("ERUPTION_DEBUG_DRAWSTATS") != nullptr;
    static std::vector<std::pair<uint32_t,uint32_t>> dbgPerMesh;
    if (kDbgStatsAlloc) dbgPerMesh.assign(m_meshes.size(), {0u, 0u});
    else dbgPerMesh.clear();

    VkBuffer runVb = VK_NULL_HANDLE, runIb = VK_NULL_HANDLE;
    uint32_t runCount = 0, runFirst = 0, runLen = 0, runMesh = 0;
    float runAlpha = 1.0f;
    bool runNear = false;   // instancias deste run estao na banda de tesselacao

    // TESSELACAO NA BANDA PERTO. Troca de pipeline por RUN, e so' para malha de
    // CHAO com a camera dentro da banda (m_tessBandDistance > 0): o chao e' o
    // que fica "absurdamente perto" na pratica - medido, na pose de 70 u o
    // frame tem UMA instancia visivel e ela e' o terreno. Prop e folhagem
    // continuam no pipeline normal: cartao com recorte por alfa nao tem relevo
    // para deslocar e so' pagaria os dois estagios a mais.
    bool tessBound = false;
    auto bindPipelineFor = [&](bool wantTess) {
        if (wantTess == tessBound) return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          wantTess ? m_tessPipeline : m_pipeline);
        tessBound = wantTess;
    };

    auto flushRun = [&]() {
        if (runLen == 0) return;
        const auto& mesh = m_meshes[runMesh];
        // O chao e' UMA malha gigante: nele o corte e' por CELULA (so' o pedaco
        // perto vai pro patch). Os demais objetos sao pequenos e o corte por
        // instancia ja' basta.
        const bool tessCells = tessellationActive() && mesh.isGround &&
                               m_tessBandDistance > 0.0f && !mesh.groundCells.empty();
        const bool wantTess = tessCells;
        if (!tessCells) bindPipelineFor(tessellationActive() && runNear);
        push.alpha = runAlpha;
        push.metallicScale = mesh.metallicScale;
        push.roughnessScale = mesh.roughnessScale;
        push.uvScale.w = mesh.dispScale;
        vkCmdPushConstants(cmd, m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                           VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                           0, sizeof(push), &push);
        if (runVb != boundVb || runIb != boundIb) {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &runVb, &offset);
            vkCmdBindIndexBuffer(cmd, runIb, 0, VK_INDEX_TYPE_UINT32);
            boundVb = runVb;
            boundIb = runIb;
        }
        if (wantTess) {
            // CHAO NA BANDA: um desenho por celula. As celulas dentro da banda
            // vao pelo pipeline de PATCH (tesselacao + deslocamento); as de
            // fora voltam pro pipeline normal. Assim o estagio de tesselacao
            // so' ve' o pedaco perto - a malha inteira custava 304 ms.
            const Vec3 camXZ = m_lastCameraPos;
            const float band = m_tessBandDistance;
            uint32_t segFirst = 0, segCount = 0;
            bool segNear = false, segOpen = false;
            auto flushSeg = [&]() {
                if (!segOpen || segCount == 0) return;
                bindPipelineFor(segNear);
                vkCmdDrawIndexed(cmd, segCount, runLen, segFirst, 0, instBase + runFirst);
                dbgDraws++;
                dbgIdx += segCount * runLen;
                segCount = 0;
                segOpen = false;
            };
            for (const auto& c : mesh.groundCells) {
                const float dx = std::max({c.minX - camXZ.x, 0.0f, camXZ.x - c.maxX});
                const float dz = std::max({c.minZ - camXZ.z, 0.0f, camXZ.z - c.maxZ});
                const bool nearCell = (dx * dx + dz * dz) < band * band;
                // Funde com o intervalo aberto quando o veredito e' o mesmo E os
                // indices sao contiguos (as celulas estao em ordem de buffer).
                if (segOpen && segNear == nearCell && segFirst + segCount == c.first) {
                    segCount += c.count;
                    continue;
                }
                flushSeg();
                segFirst = c.first;
                segCount = c.count;
                segNear = nearCell;
                segOpen = true;
            }
            flushSeg();
            if (dbgPerMesh.size() > runMesh) {
                dbgPerMesh[runMesh].first += (runCount / 3) * runLen;
                dbgPerMesh[runMesh].second += runLen;
            }
            runLen = 0;
            return;
        }
        vkCmdDrawIndexed(cmd, runCount, runLen, 0, 0, instBase + runFirst);
        dbgDraws++;
        dbgIdx += runCount * runLen;
        if (dbgPerMesh.size() > runMesh) {
            dbgPerMesh[runMesh].first += (runCount / 3) * runLen;
            dbgPerMesh[runMesh].second += runLen;
        }
        runLen = 0;
    };

    // CROSSFADE DE LOD, parte 2: instancias na banda de transicao (contadas
    // no pre-passe acima) ganham, alem do draw normal abaixo, uma entrada
    // aqui - desenhada numa SEGUNDA passada pequena logo depois, com o nivel
    // de LOD VIZINHO e o padrao de dither COMPLEMENTAR (ver o comentario em
    // model.frag). O sinal de `alpha` carrega a direcao do teste: LOD "perto"
    // (cheio) sempre positivo, LOD "longe" (reduzido) sempre negativo -
    // funciona nos dois sentidos (perto->longe OU longe->perto) porque so'
    // depende de QUAL DOS DOIS cada draw representa, nao de qual o
    // sortVisibleByMesh escolheu como "principal" para o frame.
    struct LodFadeJob {
        uint32_t meshIndex;
        uint8_t lod;     // do VIZINHO (1 ou 2) - nunca igual ao lod primario desta instancia
        float alpha;     // sinal codifica o lado (ver model.frag)
        GpuModelInstance data;
    };
    std::vector<LodFadeJob> fadeJobs;
    if (companionCount > 0) fadeJobs.reserve(companionCount * 2u); // 2 jobs por instancia em fade

    // So' instrumenta o passe PRINCIPAL: o minimapa roda no mesmo frame com
    // m_forceBaseLod (todo mundo em LOD cheio), e misturar os dois faria cada
    // frame acusar a cena inteira "trocando de nivel" duas vezes.
    static const bool kLodPopDbg = std::getenv("ERUPTION_DEBUG_LODPOP") != nullptr;
    const bool popDbg = kLodPopDbg && !m_forceBaseLod;
    static uint32_t s_popFrame = 0;
    if (popDbg) ++s_popFrame;
    uint32_t dbgPops = 0, dbgHardSwitches = 0;

    // Buffers de um NIVEL qualquer (0=hi, 1=cheio, 2=reduzido, 3=far). O
    // crossfade precisa disto para o nivel VIZINHO tambem, entao virou lambda
    // em vez de ficar so' inline no laco - o nivel 0 troca tambem o VERTEX
    // buffer, nao so' o de indices.
    auto pickBuffers = [](const ModelMeshGPU& m, uint8_t level,
                          VkBuffer& vb, VkBuffer& ib, uint32_t& count) {
        vb = m.vertexBuffer;
        ib = m.indexBuffer;
        count = m.indexCount;
        if (level == 0 && m.hiIndexCount > 0) {
            vb = m.hiVertexBuffer; ib = m.hiIndexBuffer; count = m.hiIndexCount;
        } else if (level == 2 && m.loIndexCount > 0) {
            if (m.loVertexBuffer != VK_NULL_HANDLE) vb = m.loVertexBuffer;
            ib = m.loIndexBuffer; count = m.loIndexCount;
        } else if (level == 3 && m.farIndexCount > 0) {
            if (m.farVertexBuffer != VK_NULL_HANDLE) vb = m.farVertexBuffer;
            ib = m.farIndexBuffer; count = m.farIndexCount;
        }
    };

    uint32_t slot = 0;
    for (uint32_t idx : m_visibleInstanceIndices) {
        // NAO le m_instances aqui: e' o struct frio de 208 B (ver
        // m_instUvTranslateRot). Tudo que o desenho precisa esta' no Hot e
        // nos espelhos paralelos.
        const ModelInstanceHot& hot = m_instanceHot[idx];
        const auto& mesh = m_meshes[hot.meshIndex];

        // LOD geométrico: perto usa a malha subdividida + passa-baixa, longe
        // usa a original. Ambas já existem na GPU desde o load, então trocar
        // é só escolher o buffer - zoom in/out não regenera nada e não trava.
        // O LOD ja' foi decidido em sortVisibleByMesh, que agrupou por ele.
        // Recalcular aqui com outro criterio faria o desenho divergir do
        // agrupamento e quebraria os runs - por isso e' CONSUMIDO, nao refeito.
        const uint8_t lod = (idx < m_lodOfInstance.size()) ? m_lodOfInstance[idx] : 1;
        VkBuffer vb, ib;
        uint32_t drawCount;
        pickBuffers(mesh, lod, vb, ib, drawCount);
        if (m_hiLodActive.size() > idx) m_hiLodActive[idx] = (lod == 0);
        if (m_loLodActive.size() > idx) m_loLodActive[idx] = (lod == 2);

        // Dados por instancia -> SSBO (na ordem da lista visivel).
        GpuModelInstance& d = dst[slot];
        d.model = hot.transform;
        d.uvTranslateRot = m_instUvTranslateRot[idx];
        d.uvScaleDisp = Vec4(m_instUvScale[idx], 0.0f, mesh.swayAmount);
        // Banda perto do displacement (fade POR VÉRTICE no shader; a CPU só
        // entrega amplitude + alcance, e instância além da banda nem paga).
        if (m_geoDispAmplitude > 0.0f && m_geoDispDistance > 0.0f) {
            const Vec3 center = (hot.worldAabbMin + hot.worldAabbMax) * 0.5f;
            const float radius = glm::length(hot.worldAabbMax - hot.worldAabbMin) * 0.5f;
            const float dd = glm::distance(cameraPos, center) - radius;
            if (dd < m_geoDispDistance && radius >= 100.0f) {
                d.uvScaleDisp.z = m_geoDispAmplitude;
                d.uvTranslateRot.w = m_geoDispDistance;
            }
        }

        // Realce do editor: instancia destacada opaca, resto a 20%. Alpha e'
        // por RUN (push), entao runs quebram por alpha - so' no modo realce.
        float alpha = highlight
            ? ((m_highlightedInstanceIndex == static_cast<int>(idx)) ? 1.0f : 0.2f)
            : 1.0f;

        // CROSSFADE DE LOD, parte 1: se esta instancia esta' na banda de
        // transicao (mesmo criterio do pre-passe de contagem acima), o draw
        // PRIMARIO (o `lod` que o sortVisibleByMesh ja' decidiu) so' cobre
        // uma FRACAO da tela - o resto vem do vizinho, agendado como job pra
        // segunda passada. Desligado no modo de realce do editor (as duas
        // semanticas de alpha colidiriam).
        // NIVEL EFETIVO continuo: 1.0 = cheio inteiro, 2.0 = reduzido
        // inteiro, 1.4 = 40% do caminho entre os dois. E' o que o
        // ERUPTION_DEBUG_LODPOP compara entre frames - com o crossfade
        // funcionando ele anda no maximo um degrau de quantizacao por frame.
        float effectiveLevel = static_cast<float>(lod);
        bool skipPrimary = false;

        if (!highlight && !kLodFadeOff && !m_forceBaseLod) {
            const LodFadePair pair = lodFadePairFor(mesh, lodSwitchFor(hot, mesh),
                                                    lodDistanceFor(idx, hot));
            // So' entra se o nivel que o sort escolheu e' um dos dois do par:
            // se divergirem (mesh com bandas sobrepostas, config estranha),
            // desenha o degrau duro de sempre em vez de misturar niveis
            // errados.
            if (pair.active && (lod == pair.nearLevel || lod == pair.farLevel)) {
                // t: 0 no nivel de PERTO inteiro -> 1 no nivel de LONGE
                // inteiro. threshold e' o MESMO numero pros dois desenhos -
                // so' o sinal muda o lado do dither que cada um cobre (ver
                // model.frag): perto = positivo, longe = negativo.
                float t = glm::clamp(pair.t, 0.0f, 1.0f);
                t = std::round(t * kLodFadeSteps) / kLodFadeSteps;
                effectiveLevel = static_cast<float>(pair.nearLevel) + t;
                // t nos EXTREMOS nao e' transicao: e' um nivel inteiro, e tem
                // que sair identico ao caminho sem crossfade (alpha 1.0, sem
                // companheiro). Como a banda e' exatamente a faixa de
                // histerese, t=0 <=> nivel de perto e t=1 <=> nivel de longe
                // ja' sao o que a histerese escolheria - basta nao mexer.
                if (t > 0.0f && t < 1.0f) {
                    // A instancia em transicao SAI da passada principal e vai
                    // INTEIRA pra segunda (dois jobs: perto com +limiar, longe
                    // com -limiar). Antes o primario desenhava um dos lados
                    // com alpha != 1 e isso QUEBRAVA O RUN em que ela estava:
                    // num campo de mato (parana_field regenerado, 19k
                    // instancias, 149 na banda) 39 draws viraram 304, porque
                    // cada instancia em fade partia o run da malha dela em
                    // dois. Fora da passada principal o run fica intacto, e a
                    // segunda passada agrupa por (malha, nivel, alpha
                    // quantizado) apos um sort - nao por ordem de visibilidade.
                    const float threshold = 1.0f - t;
                    LodFadeJob job;
                    job.meshIndex = hot.meshIndex;
                    job.data.model = hot.transform;
                    job.data.uvTranslateRot = m_instUvTranslateRot[idx];
                    job.data.uvScaleDisp = Vec4(m_instUvScale[idx], 0.0f, mesh.swayAmount);
                    job.lod = pair.nearLevel; job.alpha = threshold;
                    fadeJobs.push_back(job);
                    job.lod = pair.farLevel;  job.alpha = -threshold;
                    fadeJobs.push_back(job);
                    skipPrimary = true;
                }
            }
        }

        // ERUPTION_DEBUG_LODPOP=1: conta quantas instancias SALTARAM a
        // cobertura de um frame pro outro. Com o crossfade funcionando, o
        // movimento suave da camera anda no maximo um degrau de quantizacao
        // por frame; um salto maior e' um degrau duro - e' isso que o olho
        // ve' como "jitter". Separa os saltos do par cheio<->reduzido (que o
        // crossfade deveria cobrir) dos que envolvem hi/far (que ainda nao
        // tem crossfade nenhum - escopo declarado em docs/pedidos.md).
        if (popDbg) {
            if (m_lodCoveragePrev.size() != m_instances.size()) {
                m_lodCoveragePrev.assign(m_instances.size(), 0.0f);
                // Sentinela: 0xFFFFFFFF+1 nunca casa com frame algum, entao o
                // primeiro frame nao acusa a cena inteira como "pop" (era o
                // que acontecia com 0: 0+1 == frame 1 pra todo mundo).
                m_lodSeenFrame.assign(m_instances.size(), 0xFFFFFFFFu);
            }
            // So' compara com quem estava visivel no frame IMEDIATAMENTE
            // anterior: quem acabou de entrar no campo de visao nao "pulou"
            // nada, so' apareceu.
            if (m_lodSeenFrame[idx] + 1 == s_popFrame) {
                const float delta = std::fabs(effectiveLevel - m_lodCoveragePrev[idx]);
                // Salto maior que um degrau de quantizacao (com folga) = o
                // olho ve' a malha trocar. Meio nivel ou mais = troca dura,
                // sem crossfade nenhum no meio.
                if (delta > 1.5f / kLodFadeSteps) ++dbgPops;
                if (delta >= 0.5f) ++dbgHardSwitches;
            }
            m_lodCoveragePrev[idx] = effectiveLevel;
            m_lodSeenFrame[idx] = s_popFrame;
        }

        // Em transicao: nao ocupa slot nem entra em run aqui (ver acima).
        if (skipPrimary) continue;

        // BANDA DE TESSELACAO POR INSTANCIA. Vale para QUALQUER objeto, nao so'
        // o chao (pedido do autor 2026-09-06: "e em TODOS OS OBJETOS no range
        // da camera"). A instancia entra se a caixa dela alcanca a banda; a
        // curva no shader ainda decide o fator por vertice, entao um objeto
        // grande atravessando a fronteira e' tesselado so' na parte perto.
        // Instancias fora da banda continuam no pipeline normal e nao pagam o
        // estagio - e' o que segurou o custo (a malha inteira do chao pelo
        // caminho de patch custava 304 ms).
        bool instNear = false;
        if (m_tessEnabled && m_tessBandDistance > 0.0f) {
            const Vec3 c = (hot.worldAabbMin + hot.worldAabbMax) * 0.5f;
            const float r = glm::length(hot.worldAabbMax - hot.worldAabbMin) * 0.5f;
            // BANDA PROPRIA, MAIS CURTA, PARA FOLHAGEM.
            //
            // A folhagem custa desproporcionalmente: medido em parana_demo com
            // a camera junto do mato, mandar a folhagem inteira para o patch
            // levava o G-buffer de 4,87 para 13,86 ms - 5,90x mais triangulo
            // rasterizado para 1,08x de fragmento.
            //
            // Mas ela NAO ganha "nada", ao contrario do que a nota supunha: a
            // subdivisao CURVA a lamina da folha. Sem ela o cartao volta a ser
            // reto e facetado, com aresta de poligono visivel de perto (medido
            // e olhado: 72% dos pixels iluminados mudam a 75 u, com 32% de
            // contraste relativo). Cortar a folhagem inteira foi longe demais.
            //
            // A saida e' distancia, e ela devolve POUCO mas de GRACA. Padrao
            // 0,25: varrida a distancia inteira em parana_demo, a diferenca
            // para a banda cheia e' 0,000% dos pixels iluminados abaixo de
            // 110 u - justamente onde a curvatura da lamina se le' - e o pior
            // caso da varredura inteira e' 0,144%. Economiza 0,96 ms na pose
            // colada e 0,16 ms na orbita de bancada.
            //
            // NAO espere mais que isso daqui: a folhagem que CUSTA e' a mais
            // perto, que e' a mesma que se VE'. Cortar a folhagem inteira
            // (banda 0) devolve 8,9 ms e achata a lamina - 72% dos pixels
            // iluminados mudam a 75 u. Ja' foi tentado e revertido, ver
            // docs/tefra/basalto-tessellation-medido.md §11.
            // ERUPTION_TESS_FOLIAGE_BAND (0 = nenhuma folhagem tesselada,
            // 1 = mesma banda do resto).
            static const float kFoliageBand = [] {
                const char* e = std::getenv("ERUPTION_TESS_FOLIAGE_BAND");
                return e ? std::max(0.0f, static_cast<float>(std::atof(e))) : 0.25f;
            }();
            float band = m_tessBandDistance;
            if (m_meshes[hot.meshIndex].swayAmount > 0.0f) band *= kFoliageBand;
            instNear = band > 0.0f &&
                       (glm::distance(m_lastCameraPos, c) - r) < band;
        }
        const bool sameRun = runLen > 0 && vb == runVb && ib == runIb &&
                             drawCount == runCount && hot.meshIndex == runMesh &&
                             alpha == runAlpha && instNear == runNear;
        if (!sameRun) {
            flushRun();
            runVb = vb;
            runIb = ib;
            runCount = drawCount;
            runMesh = hot.meshIndex;
            runFirst = slot;
            runAlpha = alpha;
            runNear = instNear;
        }
        ++runLen;
        ++slot;
    }
    flushRun();

    // CROSSFADE DE LOD, parte 3: segunda passada pequena, so' para os jobs
    // agendados acima. Escreve nos slots LOGO APOS os da passada principal
    // (dst2 = dst + slot) e desenha agrupando por (malha, nivel, alpha) -
    // mesma logica de runs de cima, so' que sobre uma lista muito menor
    // (so' quem esta' de fato na banda de transicao neste frame), entao um
    // sort simples basta.
    uint32_t actualCompanions = static_cast<uint32_t>(fadeJobs.size());
    if (actualCompanions > 0) {
        std::sort(fadeJobs.begin(), fadeJobs.end(), [](const LodFadeJob& a, const LodFadeJob& b) {
            if (a.meshIndex != b.meshIndex) return a.meshIndex < b.meshIndex;
            if (a.lod != b.lod) return a.lod < b.lod;
            return a.alpha < b.alpha; // alpha quantizado (kLodFadeSteps): agrupa runs
        });
        GpuModelInstance* dst2 = dst + slot; // logo apos os slots que o primario USOU (quem fez fade nao ocupou)
        VkBuffer runVb2 = VK_NULL_HANDLE, runIb2 = VK_NULL_HANDLE;
        uint32_t runCount2 = 0, runFirst2 = 0, runLen2 = 0, runMesh2 = 0;
        float runAlpha2 = 1.0f;
        auto flushRun2 = [&]() {
            if (runLen2 == 0) return;
            const auto& mesh = m_meshes[runMesh2];
            push.alpha = runAlpha2;
            push.metallicScale = mesh.metallicScale;
            push.roughnessScale = mesh.roughnessScale;
            vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            if (runVb2 != boundVb || runIb2 != boundIb) {
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &runVb2, &offset);
                vkCmdBindIndexBuffer(cmd, runIb2, 0, VK_INDEX_TYPE_UINT32);
                boundVb = runVb2;
                boundIb = runIb2;
            }
            vkCmdDrawIndexed(cmd, runCount2, runLen2, 0, 0, instBase + slot + runFirst2);
            dbgDraws++;
            dbgIdx += runCount2 * runLen2;
            runLen2 = 0;
        };
        for (uint32_t i = 0; i < actualCompanions; ++i) {
            const LodFadeJob& job = fadeJobs[i];
            const auto& mesh = m_meshes[job.meshIndex];
            dst2[i] = job.data;
            // Nivel do vizinho pode ser qualquer um dos quatro (hi<->cheio,
            // cheio<->reduzido, reduzido<->far), e o hi troca tambem o VERTEX
            // buffer - por isso a mesma lambda do laco principal.
            VkBuffer vb2, ib2;
            uint32_t drawCount2;
            pickBuffers(mesh, job.lod, vb2, ib2, drawCount2);
            const bool sameRun2 = runLen2 > 0 && vb2 == runVb2 && ib2 == runIb2 &&
                                  drawCount2 == runCount2 && job.meshIndex == runMesh2 &&
                                  job.alpha == runAlpha2;
            if (!sameRun2) {
                flushRun2();
                runVb2 = vb2;
                runIb2 = ib2;
                runCount2 = drawCount2;
                runMesh2 = job.meshIndex;
                runFirst2 = i;
                runAlpha2 = job.alpha;
            }
            ++runLen2;
        }
        flushRun2();
    }
    m_instCursor += slot + actualCompanions; // = visN + (instancias em fade)
    // ERUPTION_DEBUG_LODFADE=1: um log por frame com jobs>0 - diagnostico
    // pontual pra confirmar que a banda de transicao esta' sendo cruzada
    // (o log periodico do ERUPTION_DEBUG_DRAWSTATS so' imprime a cada 120
    // frames, o que pode nao coincidir com o instante do teste).
    if (actualCompanions > 0 && std::getenv("ERUPTION_DEBUG_LODFADE")) {
        ERUPTION_LOG_WARN("[LODFADE] jobs=%u visiveis=%u", actualCompanions, visN);
    }
    if (popDbg && (dbgPops > 0 || dbgHardSwitches > 0)) {
        ERUPTION_LOG_WARN("[LODPOP] frame=%u pops=%u trocas_duras_hi_far=%u fade=%u visiveis=%u",
                          s_popFrame, dbgPops, dbgHardSwitches, actualCompanions, visN);
    }

    // Triangulos e draws REALMENTE submetidos neste frame (pos-culling, pos-LOD).
    // dbgDraws/dbgIdx ja eram acumulados sempre - so' o log e' que era gateado -
    // entao expor isto na telemetria nao custa nada por frame.
    m_lastDrawnTriangles = dbgIdx / 3;
    m_lastDrawCalls = dbgDraws;

    // ERUPTION_DEBUG_DRAWSTATS=1: quantos draws e triangulos o passe principal
    // realmente submete por frame. Sem isso a discussao "CPU-bound no dispatch"
    // vs "GPU-bound em geometria" fica no chute.
    static const bool kDrawStats = std::getenv("ERUPTION_DEBUG_DRAWSTATS") != nullptr;
    if (kDrawStats) {
        static int f = 0;
        if (++f % 120 == 0) {
            ERUPTION_LOG_WARN("[DRAWSTATS] instancias=%zu visiveis=%zu draws=%u triangulos=%u malhas=%zu descartados_tela=%u pxUnit=%.0f minPx=%.2f lodFadeJobs=%u",
                              m_instances.size(), m_visibleInstanceIndices.size(),
                              dbgDraws, dbgIdx / 3, m_meshes.size(),
                              m_lastScreenCulled, m_pixelsPerUnit, m_screenCullMinPx,
                              actualCompanions);
            // Quem PAGA o passe, ponderado por instancia. Medido no render:
            // um log no build das malhas reporta zero instancias, porque elas
            // ainda nem existem la'.
            std::vector<std::pair<uint32_t,uint32_t>> top;
            for (uint32_t i = 0; i < dbgPerMesh.size(); ++i)
                if (dbgPerMesh[i].first) top.push_back({dbgPerMesh[i].first, i});
            std::sort(top.rbegin(), top.rend());
            for (size_t k = 0; k < 12 && k < top.size(); ++k) {
                const auto& mm = m_meshes[top[k].second];
                float red = mm.loIndexCount ? 100.0f * (1.0f - float(mm.loIndexCount) / float(mm.indexCount)) : -1.0f;
                ERUPTION_LOG_WARN("   malha %u: %u tris desenhados / %u instancias | cheia=%u LOD=%u (reduz %.0f%%) troca=%.0f",
                                  top[k].second, top[k].first, dbgPerMesh[top[k].second].second,
                                  mm.indexCount / 3, mm.loIndexCount / 3, red, mm.loSwitchDistance);
            }
        }
    }
}

void ModelRenderer::renderShadow(VkCommandBuffer cmd, VkPipeline shadowPipeline, VkPipelineLayout shadowLayout,
                                 const Mat4& cascadeMatrix, const Frustum& shadowFrustum,
                                 float minSizeThreshold, bool enableCulling, bool instanced,
                                 const Vec4& windParams, VkPipeline opaquePipeline) {
    if (!m_initialized || m_instances.empty()) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);

    // Culling por frustum da LUZ (nunca o da câmera): caster fora da tela ainda
    // projeta sombra visível. Regra do projeto: se tem sombra, mantenha.
    //
    // OFENSOR #1 do relatorio de memoria: isto escaneava m_instances (AoS de
    // 208B) do zero a cada chamada - 2-3x por frame (cascatas + chuva). O cull
    // da CAMERA ja' tinha o par SoA/Hot certo para isto (cullAABBSoA, AVX2,
    // 8 instancias por instrucao); a sombra so' nao reusava. Agora reusa:
    // ensureInstanceSoAFresh() garante que o SoA reflete o Hot atual (o
    // rebuild so' roda de verdade uma vez por frame, nao importa quem pede
    // primeiro), o teste geometrico de frustum roda em SIMD sobre TODAS as
    // instancias de uma vez, e o loop abaixo so' toca ModelInstanceHot
    // (96-100B, 2 linhas de cache) para os filtros escalares - nunca mais o
    // ModelInstance de 208B.
    static const bool kProf2 = std::getenv("ERUPTION_TEST_SHADOW_DEBUG") != nullptr;
    auto t0 = std::chrono::steady_clock::now();
    const uint32_t instCount = static_cast<uint32_t>(m_instanceHot.size());
    m_shadowVisibleScratch.clear();
    m_shadowVisibleScratch.reserve(instCount);
    if (m_shadowCulledPrev.size() != instCount)
        m_shadowCulledPrev.assign(instCount, 0);

    ensureInstanceSoAFresh();
    const uint8_t* frustumMask = nullptr;
    if (enableCulling) {
        const uint32_t maskBytes = (instCount + 7) / 8;
        m_shadowMaskScratch.assign(maskBytes, 0);
        FrustumPlanesSoA lightPlanes = convertFrustumToSoA(shadowFrustum);
        cullAABBSoA(
            m_instanceSoA.minX.data(), m_instanceSoA.minY.data(), m_instanceSoA.minZ.data(),
            m_instanceSoA.maxX.data(), m_instanceSoA.maxY.data(), m_instanceSoA.maxZ.data(),
            m_paddedInstanceCount, lightPlanes, m_shadowMaskScratch.data());
        frustumMask = m_shadowMaskScratch.data();
    }

    for (uint32_t i = 0; i < instCount; ++i) {
        if (frustumMask && !((frustumMask[i / 8] & (1u << (i & 7))) != 0)) continue;
        const ModelInstanceHot& hot = m_instanceHot[i];
        if (!(hot.flags & 1u)) continue; // enabled
        if (m_shadowGroundFilter != 0 && hot.meshIndex < m_meshes.size()) {
            // G38: bake das probes separa chao de resto (ver setShadowGroundFilter).
            bool ground = m_meshes[hot.meshIndex].isGround;
            if (!ground && m_shadowGroundHugeXZ > 0.0f) {
                const Vec3 ext = hot.worldAabbMax - hot.worldAabbMin;
                ground = ext.x > m_shadowGroundHugeXZ || ext.z > m_shadowGroundHugeXZ;
            }
            if ((m_shadowGroundFilter == 1) != ground) continue;
        }
        if (minSizeThreshold > 0.0f && hot.boundingRadius < minSizeThreshold) continue;
        // Tamanho projetado NA TELA: uma sombra de menos de m_shadowMinPx px nao
        // e' visivel; sem isto o criterio era so' por zoom (curva) e em cidade-A
        // props inteiros perdiam a sombra de uma vez ao afastar o zoom.
        if (m_shadowMinPx > 0.0f && m_pixelsPerUnit > 0.0f) {
            const Vec3 c = (hot.worldAabbMin + hot.worldAabbMax) * 0.5f;
            const float d = std::max(glm::distance(m_lastCameraPos, c), 1.0f);
            const float px = hot.boundingRadius * m_pixelsPerUnit / d;
            // HISTERESE. Sem ela, um objeto parado exatamente no limiar liga e
            // desliga a propria sombra a cada frame conforme a camera se move,
            // e o piscar e' MUITO visivel (relatado pelo autor 2026-09-03:
            // "a sombra ta com um culling fodido quando eu movo a camera").
            // Quem ja' esta' descartado so' volta com 40% de folga; quem esta'
            // desenhando so' sai abaixo do limiar cheio. Assim a transicao
            // acontece uma vez, nao a cada frame.
            const bool jaCortado = m_shadowCulledPrev[i] != 0;
            const float limiar = jaCortado ? m_shadowMinPx * 1.4f : m_shadowMinPx;
            const bool cortar = px < limiar;
            m_shadowCulledPrev[i] = cortar ? 1 : 0;
            if (cortar) continue;
        }
        m_shadowVisibleScratch.push_back(i);
    }
    // ERUPTION_DEBUG_SHADOWCASTERS=1: casters por chamada (uma por cascata) -
    // um salto no numero entre frames vizinhos e' o "pop" de culling.
    static const bool kCastersDbg = std::getenv("ERUPTION_DEBUG_SHADOWCASTERS") != nullptr;
    if (kCastersDbg) ERUPTION_LOG_WARN("[CASTERS] %zu de %u (minSize %.2f, minPx %.2f)",
                                       m_shadowVisibleScratch.size(), instCount, minSizeThreshold, m_shadowMinPx);
    auto t1 = std::chrono::steady_clock::now();
    // Agrupado por malha os draws instanciados saem longos; a sombra paga por
    // cascata, entao o ganho e' multiplicado.
    sortVisibleByMesh(m_shadowVisibleScratch, /*useLod=*/false, /*recordStats=*/false);
    // SONDA: ERUPTION_TEST_SHADOW_REVERSE=1 inverte a ordem de desenho dos
    // casters. O passe de sombra so' escreve PROFUNDIDADE, entao a ordem NAO
    // pode mudar a imagem - se ela mudar o TEMPO, e' porque o hardware esta'
    // fazendo teste antecipado de Z (o `discard` do shadow.frag impede o
    // WRITE antecipado, nao o TEST), e ai' ordenar frente-para-tras em espaco
    // de luz tem premio. Se o tempo nao mudar, nao tem, e a ideia morre aqui
    // sem eu implementar a ordenacao inteira.
    static const bool kShadowReverse = [] {
        const char* e = std::getenv("ERUPTION_TEST_SHADOW_REVERSE");
        return e && std::atoi(e) != 0;
    }();
    if (kShadowReverse) {
        std::reverse(m_shadowVisibleScratch.begin(), m_shadowVisibleScratch.end());
    }
    auto t2 = std::chrono::steady_clock::now();
    m_shadowDrawCallsDbg = 0;
    if (kProf2) {
        static int fr=0; static double aCull=0,aSort=0;
        aCull += std::chrono::duration<double,std::milli>(t1-t0).count();
        aSort += std::chrono::duration<double,std::milli>(t2-t1).count();
        if (++fr % 120 == 0) {
            size_t uniq = 0, trans = 0;
            uint32_t prev = UINT32_MAX;
            std::unordered_set<uint32_t> um;
            for (uint32_t idx : m_shadowVisibleScratch) {
                const uint32_t m = m_instanceHot[idx].meshIndex;
                um.insert(m);
                if (m != prev) { ++trans; prev = m; }
            }
            uniq = um.size();
            ERUPTION_LOG_WARN("[SHPROF2] cull=%.2f sort=%.2f n=%zu de %zu | meshes unicos=%zu transicoes=%zu",
                              aCull/120, aSort/120, m_shadowVisibleScratch.size(), m_instances.size(), uniq, trans);
            aCull=aSort=0;
        }
    }

    // ---- INSTANCING da sombra ----
    // Antes: um vkCmdPushConstants(MVP) + um vkCmdDrawIndexed POR CASTER POR
    // CASCATA (~4,9 ms de CPU em parana_field). Agora: a matriz de cada caster
    // vai num SSBO com cursor de append (cada cascata escreve seu trecho), o
    // cascadeVP e' UM push por cascata, e cada malha vira um draw instanciado.
    const uint32_t n = static_cast<uint32_t>(m_shadowVisibleScratch.size());
    if (n == 0) return;
    if (!instanced || !ensureShadowCapacity()) {
        // Caminho por draw: MVP por push, instanceCount=1. Usado pelo passe de
        // profundidade top-down da chuva, que roda com o pipeline simples.
        static const bool kFullDetailFb = std::getenv("ERUPTION_SHADOW_FULL_DETAIL") != nullptr;
        VkBuffer bvb = VK_NULL_HANDLE, bib = VK_NULL_HANDLE;
        for (uint32_t idx : m_shadowVisibleScratch) {
            // Le do Hot (96-100B), nao mais do ModelInstance de 208B - o cull
            // logo acima ja' so' toca Hot, entao o draw fica na MESMA malha
            // de cache que o cull acabou de aquecer.
            const ModelInstanceHot& hot = m_instanceHot[idx];
            const auto& mesh = m_meshes[hot.meshIndex];
            Mat4 mvp = cascadeMatrix * hot.transform;
            vkCmdPushConstants(cmd, shadowLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(Mat4), &mvp);
            VkBuffer useIb = mesh.indexBuffer;
            VkBuffer useVb = mesh.vertexBuffer;
            uint32_t useCount = mesh.indexCount;
            if (!kFullDetailFb) {
                if (mesh.shIndexCount > 0) { useIb = mesh.shIndexBuffer; useCount = mesh.shIndexCount; }
                else if (mesh.loIndexCount > 0) {
                    useIb = mesh.loIndexBuffer; useCount = mesh.loIndexCount;
                    // Poda de cartoes: o "lo" tem vertices proprios (escalados).
                    if (mesh.loVertexBuffer != VK_NULL_HANDLE) useVb = mesh.loVertexBuffer;
                }
            }
            if (useVb != bvb || useIb != bib) {
                VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &useVb, &off);
                vkCmdBindIndexBuffer(cmd, useIb, 0, VK_INDEX_TYPE_UINT32);
                bvb = useVb; bib = useIb;
            }
            vkCmdDrawIndexed(cmd, useCount, 1, 0, 0, 0);
        }
        return;
    }
    const uint32_t frame = m_ctx->currentFrame();
    if (frame != m_shadowLastFrame) {
        m_shadowLastFrame = frame;
        m_shadowCursor = 0;
    }
    if (m_shadowCursor + n > m_shadowCapacity) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ERUPTION_LOG_ERROR("Sombra instanciada: cursor estourou (%u+%u > %u) - cascata pulada",
                               m_shadowCursor, n, m_shadowCapacity);
        }
        return;
    }
    const uint32_t fi = frame % kInstFrames;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowLayout,
                            1, 1, &m_shadowSets[fi], 0, nullptr);
    vkCmdPushConstants(cmd, shadowLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(Mat4), &cascadeMatrix);
    // Vento (mesma convencao do FrameUBO, ver model.vert applyWindSway):
    // constante para a cascata inteira, entao um push so' basta. O offset
    // (logo apos o mat4) tem que bater com o layout(offset=...) declarado em
    // shadow_inst.vert - ver comentario la'.
    vkCmdPushConstants(cmd, shadowLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       sizeof(Mat4), sizeof(Vec4), &windParams);
    Mat4* dst = static_cast<Mat4*>(m_shadowMapped[fi]) + m_shadowCursor;

    // ERUPTION_SHADOW_FULL_DETAIL=1 volta a desenhar a malha cheia na sombra.
    static const bool kShadowFullDetail = std::getenv("ERUPTION_SHADOW_FULL_DETAIL") != nullptr;
    VkBuffer boundVb = VK_NULL_HANDLE;
    VkBuffer boundIb = VK_NULL_HANDLE;
    VkBuffer runVb = VK_NULL_HANDLE, runIb = VK_NULL_HANDLE;
    uint32_t runCount = 0, runFirst = 0, runLen = 0;
    // Pipeline corrente do passe. O run ja' e' por malha, entao a opacidade e'
    // constante dentro dele e a troca acontece no maximo uma vez por run.
    bool runOpaque = false;
    VkPipeline boundPipe = shadowPipeline;

    auto flushRun = [&]() {
        if (runLen == 0) return;
        VkPipeline want = (runOpaque && opaquePipeline != VK_NULL_HANDLE)
                        ? opaquePipeline : shadowPipeline;
        if (want != boundPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
            boundPipe = want;
        }
        if (runVb != boundVb || runIb != boundIb) {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &runVb, &offset);
            vkCmdBindIndexBuffer(cmd, runIb, 0, VK_INDEX_TYPE_UINT32);
            boundVb = runVb;
            boundIb = runIb;
        }
        vkCmdDrawIndexed(cmd, runCount, runLen, 0, 0, m_shadowCursor + runFirst);
        ++m_shadowDrawCallsDbg;
        runLen = 0;
    };

    uint32_t slot = 0;
    for (uint32_t idx : m_shadowVisibleScratch) {
        // Hot, nao ModelInstance: mesmo motivo do caminho por-draw acima.
        const ModelInstanceHot& hot = m_instanceHot[idx];
        const auto& mesh = m_meshes[hot.meshIndex];

        // A SOMBRA usa o LOD sempre que existir. Uma silhueta projetada num
        // shadow map nao precisa da malha cheia. Nada e' cortado: a regra do
        // projeto (se tem sombra, mantenha) e' sobre CULLING e continua
        // valendo - todo caster que projeta continua projetando.
        VkBuffer useIb = mesh.indexBuffer;
        VkBuffer useVb = mesh.vertexBuffer;
        uint32_t useCount = mesh.indexCount;
        // O CHAO PROJETA COM A MALHA CHEIA. O LOD de sombra e' agressivo de
        // proposito (mesh_lod_far/shadow_lod_max_tris: 300-800 triangulos) e
        // isso e' correto para um prop, cuja silhueta no shadow map ocupa
        // poucos texels. Aplicado ao TERRENO e' um desastre: o chao do
        // parana_field tem 498.002 triangulos e virava um caster de 400 - uma
        // casca grosseira que NAO acompanha o relevo real e, como o receptor
        // e' o mesmo chao em resolucao cheia, o teste de profundidade acusa
        // sombra em morro aberto. Sao as MANCHAS ESCURAS EM BATATA que
        // apareciam longe de qualquer arvore e mudavam de forma quando a
        // camera girava (autor 2026-09-06: "sombra estranha toda porosa nos
        // vaos... da pra ver bem no terceiro angulo, quando rotaciona").
        // Medido: fator de sombra medio 0,44 com o LOD contra 0,28 com a
        // malha cheia - o LOD INVENTAVA sombra, nao economizava silhueta.
        // DIAGNOSTICO: ERUPTION_TEST_SHADOW_NO_GROUND=1 tira o chao do passe de
        // sombra para medir a fatia dele. NAO e' caminho de producao - sem o
        // chao nao ha' auto-sombra de relevo nenhuma.
        static const bool kNoGroundCaster = [] {
            const char* e = std::getenv("ERUPTION_TEST_SHADOW_NO_GROUND");
            return e && std::atoi(e) != 0;
        }();
        if (kNoGroundCaster && mesh.isGround) continue;
        // DIAGNOSTICO: ERUPTION_TEST_SHADOW_NO_FOLIAGE=1 tira a FOLHAGEM do
        // passe de sombra. Com o de cima, separa a fatia de quem precisa de
        // teste de alfa (folha) da de quem nao precisa (solido) - que e' o
        // premio maximo de um pipeline de sombra sem fragment shader.
        // NAO e' caminho de producao.
        static const bool kNoFoliageCaster = [] {
            const char* e = std::getenv("ERUPTION_TEST_SHADOW_NO_FOLIAGE");
            return e && std::atoi(e) != 0;
        }();
        if (kNoFoliageCaster && mesh.swayAmount > 0.0f) continue;
        const bool casterFullDetail = kShadowFullDetail || mesh.isGround;
        if (!casterFullDetail) {
            if (mesh.shIndexCount > 0) {          // LOD proprio da sombra
                useIb = mesh.shIndexBuffer;
                useCount = mesh.shIndexCount;
            } else if (mesh.loIndexCount > 0) {   // senao, o LOD de tela
                useIb = mesh.loIndexBuffer;
                useCount = mesh.loIndexCount;
                // Poda de cartoes: o "lo" tem vertices proprios (escalados).
                if (mesh.loVertexBuffer != VK_NULL_HANDLE) useVb = mesh.loVertexBuffer;
            }
        }

        dst[slot] = hot.transform;

        const bool sameRun = runLen > 0 && useVb == runVb &&
                             useIb == runIb && useCount == runCount &&
                             mesh.shadowOpaque == runOpaque;
        if (!sameRun) {
            flushRun();
            runVb = useVb;
            runIb = useIb;
            runCount = useCount;
            runFirst = slot;
            runOpaque = mesh.shadowOpaque;
            // swayAmount e' POR MALHA (mesh.swayAmount), e um run instanciado
            // e' sempre da MESMA malha (agrupado por vertex/index buffer
            // acima) - um push por inicio de run basta, nao precisa ir no
            // SSBO por instancia. Sem isto o shader de sombra nao sabe que
            // esta malha balanca e a sombra fica cravada enquanto a copa
            // balanca no G-buffer.
            vkCmdPushConstants(cmd, shadowLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               sizeof(Mat4) + sizeof(Vec4), sizeof(float), &mesh.swayAmount);
        }
        ++runLen;
        ++slot;
    }
    flushRun();
    m_shadowCursor += n;
    if (kProf2) {
        static int fr=0; static double aLoop=0; static uint32_t aDraws=0;
        aLoop += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t2).count();
        aDraws += m_shadowDrawCallsDbg;
        if (++fr % 120 == 0) {
            ERUPTION_LOG_WARN("[SHPROF3] loop=%.2f draws=%.0f", aLoop/120, double(aDraws)/120);
            aLoop=0; aDraws=0;
        }
    }
}

void ModelRenderer::updateAnimations(float totalElapsedMs) {
    // Preenche uma vez a lista de nos consultados por asset.
    if (m_animQueriedNodes.empty() && !m_animatedNodes.empty()) {
        for (const auto& a : m_animatedNodes)
            m_animQueriedNodes[a.modelAssetIndex].push_back(a.nodeIndex);
    }
    // --- Skeleton/transform animation ---
    if (!m_animatedNodes.empty()) {
        // Matrix buffers persist across frames so a steady scene does not
        // reallocate one vector per animated asset every frame; a generation
        // stamp marks which entries were already recomputed this call.
        ++m_animGen;

        for (auto& anim : m_animatedNodes) {
            if (anim.modelAssetIndex >= m_lastModels.size()) continue;
            const ModelAsset& asset = m_lastModels[anim.modelAssetIndex];
            if (asset.animDuration <= 0.0f) continue;

            CachedAnimMatrices& cached = m_animMatrixCache[anim.modelAssetIndex];
            if (cached.gen != m_animGen) {
                cached.gen = m_animGen;
                uint32_t version = asset.versionMajor * 256 + asset.versionMinor;
                float animDurationMs = (version < 0x0202)
                    ? asset.animDuration
                    : (asset.animDuration * 1000.0f / asset.frameRate);

                if (animDurationMs <= 0.0f) {
                    cached.matrices.clear();
                    continue;
                }

                float currentTimeMs = fmod(totalElapsedMs, animDurationMs);
                float currentFrame = (version < 0x0202)
                    ? currentTimeMs
                    : (currentTimeMs / 1000.0f * asset.frameRate);

                computeAnimatedRenderMatrices(asset, currentFrame, cached.matrices,
                                              &m_animQueriedNodes[anim.modelAssetIndex]);
            }

            const auto& animatedMatrices = cached.matrices;
            if (animatedMatrices.empty() || anim.nodeIndex >= animatedMatrices.size()) continue;

            Mat4 nodeRenderMatrix = animatedMatrices[anim.nodeIndex];
            m_instances[anim.instanceIndex].transform = anim.instanceMatrix * anim.mainOffset * nodeRenderMatrix;
        }
    }

    // --- Texture UV animation (lava flow, rotating surfaces, etc.) ---
    if (!m_textureAnimatedNodes.empty()) {
        for (auto& texAnim : m_textureAnimatedNodes) {
            if (texAnim.modelAssetIndex >= m_lastModels.size()) continue;
            const ModelAsset& asset = m_lastModels[texAnim.modelAssetIndex];
            if (texAnim.nodeIndex >= asset.nodes.size()) continue;
            const ModelNode& node = asset.nodes[texAnim.nodeIndex];

            float currentFrame = 0.0f;
            int32_t animLen = 1;
            if (asset.animDuration > 0.0f && asset.frameRate > 0.0f) {
                uint32_t version = asset.versionMajor * 256 + asset.versionMinor;
                float animDurationMs = (version < 0x0202)
                    ? asset.animDuration
                    : (asset.animDuration * 1000.0f / asset.frameRate);
                if (animDurationMs > 0.0f) {
                    float currentTimeMs = fmod(totalElapsedMs, animDurationMs);
                    currentFrame = (version < 0x0202)
                        ? currentTimeMs
                        : (currentTimeMs / 1000.0f * asset.frameRate);
                    animLen = glm::max(1, static_cast<int32_t>(asset.animDuration * asset.frameRate));
                }
            }

            // Most legacy lava models only animate the first texture slot (textureId 0).
            // If multiple slots are animated we average/merge their states.
            TextureAnimState merged;
            int activeCount = 0;
            int32_t lastTextureId = -1;
            for (const auto& anim : node.animTextures) {
                if (anim.textureId == lastTextureId) continue; // already merged above
                lastTextureId = anim.textureId;
                TextureAnimState state = interpolateTextureAnimation(node, anim.textureId, currentFrame, animLen);
                if (state.active) {
                    merged.translate += state.translate;
                    merged.scale = state.scale; // override, usually 1
                    merged.rotation += state.rotation;
                    merged.active = true;
                    activeCount++;
                }
            }

            ModelInstance& inst = m_instances[texAnim.instanceIndex];
            if (merged.active) {
                inst.uvTranslate = merged.translate;
                inst.uvScale = merged.scale;
                inst.uvRotation = merged.rotation;
                inst.hasTextureAnimation = true;
            } else {
                inst.hasTextureAnimation = false;
            }
        }
    }

    // Sync hot culling data after animation updates.
    rebuildHotData();
}

void ModelRenderer::rebuildHotData() {
    const uint32_t count = static_cast<uint32_t>(m_instances.size());
    m_instanceHot.resize(count);
    m_instUvTranslateRot.resize(count);
    m_instUvScale.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const ModelInstance& inst = m_instances[i];
        ModelInstanceHot& hot = m_instanceHot[i];
        // Espelha a UV aqui (ponto unico de sincronia, roda tambem depois da
        // animacao de textura) para o laco de desenho nao tocar no struct frio.
        m_instUvTranslateRot[i] = inst.hasTextureAnimation
            ? Vec4(inst.uvTranslate, inst.uvRotation, 0.0f) : Vec4(0.0f);
        m_instUvScale[i] = inst.hasTextureAnimation ? inst.uvScale : Vec2(1.0f);
        hot.transform = inst.transform;
        hot.worldAabbMin = inst.worldAabbMin;
        hot.worldAabbMax = inst.worldAabbMax;
        hot.meshIndex = inst.meshIndex;
        hot.flags = inst.enabled ? 1u : 0u;
        hot.boundingRadius = inst.boundingRadius;
    }
    // Hot mudou (chamado 1x/frame apos animacao) - o SoA derivado dele precisa
    // ser refeito antes do proximo cull, seja o da camera ou o da sombra,
    // qualquer um que rode primeiro neste frame.
    m_instanceSoADirty = true;
}

// Garante que ModelInstanceSoA reflete o ModelInstanceHot atual, refazendo o
// espelhamento NO MAXIMO uma vez por frame. Antes desta funcao existir, o cull
// da sombra reconstruia (ou pior, ignorava por completo) esse espelhamento e
// escaneava m_instances (AoS de 208B) direto - Ofensor #1 do relatorio de
// memoria. Camera e sombra agora chamam isto, e quem chegar primeiro no
// frame paga o rebuild; o outro reusa de graca.
void ModelRenderer::ensureInstanceSoAFresh() {
    if (!m_instanceSoADirty) return;
    rebuildInstanceSoA();
    m_instanceSoADirty = false;
}

void ModelRenderer::rebuildInstanceSoA() {
    const uint32_t count = static_cast<uint32_t>(m_instanceHot.size());
    m_paddedInstanceCount = (count + 7u) & ~7u;
    m_instanceSoA.resize(m_paddedInstanceCount);

    for (uint32_t i = 0; i < count; ++i) {
        const ModelInstanceHot& hot = m_instanceHot[i];
        m_instanceSoA.minX[i] = hot.worldAabbMin.x;
        m_instanceSoA.minY[i] = hot.worldAabbMin.y;
        m_instanceSoA.minZ[i] = hot.worldAabbMin.z;
        m_instanceSoA.maxX[i] = hot.worldAabbMax.x;
        m_instanceSoA.maxY[i] = hot.worldAabbMax.y;
        m_instanceSoA.maxZ[i] = hot.worldAabbMax.z;
        m_instanceSoA.meshIndex[i] = hot.meshIndex;
        m_instanceSoA.flags[i] = hot.flags;
        m_instanceSoA.radius[i] = hot.boundingRadius;
    }

    // Pad trailing entries so the AVX2 kernel does not read garbage.
    for (uint32_t i = count; i < m_paddedInstanceCount; ++i) {
        m_instanceSoA.minX[i] = 0.0f;
        m_instanceSoA.minY[i] = 0.0f;
        m_instanceSoA.minZ[i] = 0.0f;
        m_instanceSoA.maxX[i] = 0.0f;
        m_instanceSoA.maxY[i] = 0.0f;
        m_instanceSoA.maxZ[i] = 0.0f;
        m_instanceSoA.meshIndex[i] = 0;
        m_instanceSoA.flags[i] = 0;
        m_instanceSoA.radius[i] = 0.0f;
    }
}

void ModelRenderer::cullInstances(const Frustum& frustum) {
    const uint32_t count = static_cast<uint32_t>(m_instanceHot.size());
    m_visibleInstanceIndices.clear();
    if (count == 0) return;

    ensureInstanceSoAFresh();

    const uint32_t maskBytes = (count + 7) / 8;
    m_visibleMaskScratch.assign(maskBytes, 0);
    std::vector<uint8_t>& visibleMask = m_visibleMaskScratch;
    FrustumPlanesSoA planes = convertFrustumToSoA(frustum);

    cullAABBSoA(
        m_instanceSoA.minX.data(),
        m_instanceSoA.minY.data(),
        m_instanceSoA.minZ.data(),
        m_instanceSoA.maxX.data(),
        m_instanceSoA.maxY.data(),
        m_instanceSoA.maxZ.data(),
        m_paddedInstanceCount,
        planes,
        visibleMask.data());

    // DESCARTE POR TAMANHO NA TELA ("small feature culling"): instancia cujo
    // raio projetado cabe em menos de m_screenCullMinPx pixels nao contribui
    // com forma nenhuma - so' com um triangulo sub-pixel que pisca conforme a
    // camera anda. E' o que fazia o zoom 0 do parana_field parecer "chuviscar":
    // 4354 instancias visiveis, boa parte mato/pedra de 1-2 px. Histerese de
    // 20% pra nao pipocar na borda do limiar. Raio projetado (px) =
    // raio * pixelsPerUnit / distancia.
    const bool screenCull = m_screenCullMinPx > 0.0f && m_pixelsPerUnit > 0.0f;
    const float pxIn = m_screenCullMinPx;
    const float pxOut = m_screenCullMinPx * 0.8f;
    if (screenCull && m_screenCulledPrev.size() != count) m_screenCulledPrev.assign(count, 0);
    m_lastScreenCulled = 0;

    m_visibleInstanceIndices.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t byte = visibleMask[i / 8];
        if ((byte & (1u << (i & 7))) != 0) {
            // Respect the enabled flag separately.
            if (m_instanceHot[i].flags & 1u) {
                if (screenCull) {
                    const ModelInstanceHot& hot = m_instanceHot[i];
                    const Vec3 c = (hot.worldAabbMin + hot.worldAabbMax) * 0.5f;
                    const float r = glm::length(hot.worldAabbMax - hot.worldAabbMin) * 0.5f;
                    const float d = std::max(glm::distance(m_lastCameraPos, c), 1.0f);
                    const float px = r * m_pixelsPerUnit / d;
                    const bool wasCulled = m_screenCulledPrev[i] != 0;
                    const bool cull = wasCulled ? (px < pxIn) : (px < pxOut);
                    m_screenCulledPrev[i] = cull ? 1 : 0;
                    if (cull) { ++m_lastScreenCulled; continue; }
                }
                m_visibleInstanceIndices.push_back(i);
            }
        }
    }

    // O agrupamento por malha acontece no CHAMADOR (render(), logo depois de
    // atualizar m_lastCameraPos). Havia um sortVisibleByMesh AQUI tambem -
    // sort completo duplicado por passe, e pior: rodava ANTES de
    // m_lastCameraPos ser atualizado, entao o lodLevelFor decidia LOD com a
    // camera do passe ANTERIOR e o sort do render() refazia tudo por cima.
    // (Varredura de hot paths 2026-09-02, achado #5.)
}

// Counting sort estável das instâncias por meshIndex. O(n + malhas).
// Nivel de LOD desta instancia: 0 = malha fina (hi), 1 = cheia, 2 = simplificada.
// Extraida do laco de desenho para que o SORT use exatamente o mesmo criterio -
// se divergissem, o agrupamento nao casaria com o que e' desenhado.
// Ver a declaracao em ModelRenderer.hpp. Cada banda e' EXATAMENTE o intervalo
// de histerese da fronteira correspondente em lodLevelFor - e' o que garante
// que os extremos (t=0 e t=1) coincidem com o nivel inteiro que a histerese
// ja' escolheria ali, sem degrau nas bordas. Se as duas coisas divergirem,
// volta o jitter que o autor viu ao varrer o pitch no zoom 0.
ModelRenderer::LodFadePair ModelRenderer::lodFadePairFor(const ModelMeshGPU& mesh, const LodSwitch& sw, float dist) {
    LodFadePair r;
    if (mesh.hiIndexCount > 0 && mesh.hiSwitchDistance > 0.0f) {
        const float lo = mesh.hiSwitchDistance;
        const float hi = mesh.hiSwitchDistance * kHiLodHysteresis;
        if (dist > lo && dist < hi) {
            r.active = true; r.t = (dist - lo) / (hi - lo);
            r.nearLevel = 0; r.farLevel = 1;
            return r;
        }
    }
    if (mesh.loIndexCount > 0 && sw.lo > 0.0f) {
        const float lo = sw.lo * kLoLodHysteresis;
        const float hi = sw.lo;
        if (dist > lo && dist < hi) {
            r.active = true; r.t = (dist - lo) / (hi - lo);
            r.nearLevel = 1; r.farLevel = 2;
            return r;
        }
    }
    if (mesh.farIndexCount > 0 && sw.far > 0.0f) {
        const float lo = sw.far * kFarLodHysteresis;
        const float hi = sw.far;
        if (dist > lo && dist < hi) {
            r.active = true; r.t = (dist - lo) / (hi - lo);
            r.nearLevel = 2; r.farLevel = 3;
            return r;
        }
    }
    return r;
}

ModelRenderer::LodSwitch ModelRenderer::lodSwitchFor(const ModelInstanceHot& hot,
                                                     const ModelMeshGPU& mesh) const {
    LodSwitch s;
    s.lo = mesh.loSwitchDistance;
    s.far = mesh.farSwitchDistance;
    if (m_meshLodTrisPerPx <= 0.0f || m_pixelsPerUnit <= 0.0f) return s;
    const float R = glm::length(hot.worldAabbMax - hot.worldAabbMin) * 0.5f;
    if (R <= 0.0f) return s;
    // d_L = R * pxUnit * sqrt(pi * orcamento / T_L): distancia ate' a qual o
    // nivel L ainda gasta <= orcamento triangulos por pixel projetado.
    const float k = R * m_pixelsPerUnit * std::sqrt(3.14159265f * m_meshLodTrisPerPx);
    const float floorD = std::max(m_meshLodMinDistance, 1.0f);
    if (mesh.loIndexCount > 0 && mesh.indexCount >= 3) {
        const float dBase = k / std::sqrt(float(mesh.indexCount / 3));
        const float cur = (s.lo > 0.0f) ? s.lo : 1e9f;
        s.lo = std::max(std::min(cur, dBase), floorD);
    }
    if (mesh.farIndexCount > 0 && mesh.loIndexCount >= 3) {
        const float dLo = k / std::sqrt(float(mesh.loIndexCount / 3));
        const float cur = (s.far > 0.0f) ? s.far : 1e9f;
        // far so' depois do lo, com folga para as bandas de fade (0,85) nao se
        // cruzarem: 1/0,85 = 1,18.
        s.far = std::max(std::min(cur, dLo), std::max({s.lo * 1.2f, floorD, m_meshLodFarMinDistance}));
    }
    return s;
}

// Minimo de frames que um crossfade leva para atravessar a banda inteira,
// seja qual for a velocidade da camera. 12 a 60 fps = 0,2 s: curto o
// bastante para o zoom nao parecer "lento", longo o bastante para o dither
// do crossfade nao virar pop. ERUPTION_TEST_LOD_FADE_FRAMES=0 devolve o
// comportamento antigo (so' distancia) para o A/B.
static float lodFadeMinFrames() {
    static const float k = [] {
        const char* e = std::getenv("ERUPTION_TEST_LOD_FADE_FRAMES");
        return e ? static_cast<float>(std::atof(e)) : 12.0f;
    }();
    return k;
}

float ModelRenderer::advanceLodDistance(uint32_t idx, const ModelInstanceHot& hot, const ModelMeshGPU& mesh) {
    const Vec3 c = (hot.worldAabbMin + hot.worldAabbMax) * 0.5f;
    const float r = glm::length(hot.worldAabbMax - hot.worldAabbMin) * 0.5f;
    const float trueDist = glm::distance(m_lastCameraPos, c) - r;
    if (m_lodDistShown.size() != m_instances.size()) {
        m_lodDistShown.assign(m_instances.size(), 0.0f);
        m_lodDistFrame.assign(m_instances.size(), 0xFFFFFFFFu);
    }
    const float minFrames = lodFadeMinFrames();
    // Nao estava visivel no frame anterior: nao ha' de onde "vir", entao nada
    // a suavizar - reinicia na distancia real. Mesma regra do LODPOP.
    if (minFrames <= 0.0f || m_lodDistFrame[idx] + 1 != m_lodFrame) {
        m_lodDistShown[idx] = trueDist;
        m_lodDistFrame[idx] = m_lodFrame;
        return trueDist;
    }
    m_lodDistFrame[idx] = m_lodFrame;
    // As bandas desta malha, na mesma convencao de lodFadePairFor.
    struct Band { float lo, hi; };
    Band bands[3]; int nb = 0;
    const LodSwitch sw = lodSwitchFor(hot, mesh);
    if (mesh.hiIndexCount > 0 && mesh.hiSwitchDistance > 0.0f)
        bands[nb++] = { mesh.hiSwitchDistance, mesh.hiSwitchDistance * kHiLodHysteresis };
    if (mesh.loIndexCount > 0 && sw.lo > 0.0f)
        bands[nb++] = { sw.lo * kLoLodHysteresis, sw.lo };
    if (mesh.farIndexCount > 0 && sw.far > 0.0f)
        bands[nb++] = { sw.far * kFarLodHysteresis, sw.far };
    float cur = m_lodDistShown[idx];
    if (nb == 0 || cur == trueDist) { m_lodDistShown[idx] = trueDist; return trueDist; }
    const float dir = (trueDist > cur) ? 1.0f : -1.0f;
    // Duas voltas no maximo: uma para saltar ate' a borda da proxima banda no
    // caminho, outra para dar UM passo dentro dela. Fora de banda a distancia
    // salta - so' o que esta' dentro de uma banda e' que precisa de tempo.
    for (int iter = 0; iter < 2; ++iter) {
        const Band* in = nullptr;
        for (int b = 0; b < nb; ++b) if (cur > bands[b].lo && cur < bands[b].hi) { in = &bands[b]; break; }
        if (in) {
            const float step = (in->hi - in->lo) / minFrames;
            const float d = trueDist - cur;
            cur += (std::fabs(d) <= step) ? d : dir * step;
            break;
        }
        // Fora de todas: proxima borda de banda entre cur e o alvo, se houver.
        float next = trueDist;
        for (int b = 0; b < nb; ++b) {
            const float edge = (dir > 0.0f) ? bands[b].lo : bands[b].hi;
            const bool ahead = (dir > 0.0f) ? (edge > cur && edge < next) : (edge < cur && edge > next);
            if (ahead) next = edge;
        }
        if (next == trueDist) { cur = trueDist; break; }
        cur = next + dir * 1e-3f;   // um fio para dentro da banda; a volta seguinte da' o passo
    }
    m_lodDistShown[idx] = cur;
    return cur;
}

float ModelRenderer::lodDistanceFor(uint32_t idx, const ModelInstanceHot& hot) const {
    if (idx < m_lodDistShown.size() && idx < m_lodDistFrame.size() && m_lodDistFrame[idx] == m_lodFrame)
        return m_lodDistShown[idx];
    const Vec3 c = (hot.worldAabbMin + hot.worldAabbMax) * 0.5f;
    const float r = glm::length(hot.worldAabbMax - hot.worldAabbMin) * 0.5f;
    return glm::distance(m_lastCameraPos, c) - r;
}

uint8_t ModelRenderer::lodLevelFor(uint32_t idx, const ModelMeshGPU& mesh) const {
    if (m_forceBaseLod) return 1;
    // WIREFRAME ESTILIZADO: sempre o nivel MAIS grosseiro que a malha tiver.
    // Sem isto o modo nao le' como wireframe: a vegetacao do parana_demo
    // submete ~31 M de triangulos e, em POLYGON_MODE_LINE, as arestas ficam
    // mais densas que a grade de pixels - a tela vira um bloco de verde
    // solido em vez de malha. E' escolha ESTETICA (o modo e' de
    // apresentacao), nao economia: com o LOD longe a malha aparece.
    if (wireframeMode()) {
        if (mesh.farIndexCount > 0) return 3;
        if (mesh.loIndexCount > 0) return 2;
        return 1;
    }
    const ModelInstanceHot& hot = m_instanceHot[idx];
    const float dist = lodDistanceFor(idx, hot);   // suavizada, ver advanceLodDistance
    const LodSwitch sw = lodSwitchFor(hot, mesh);

    if (mesh.hiIndexCount > 0 && mesh.hiSwitchDistance > 0.0f) {
        const bool wasHi = m_hiLodActive.size() > idx && m_hiLodActive[idx];
        const float th = wasHi ? mesh.hiSwitchDistance * kHiLodHysteresis : mesh.hiSwitchDistance;
        if (dist < th) return 0;
    }
    if (mesh.farIndexCount > 0 && sw.far > 0.0f) {
        const bool wasFar = m_lodOfInstance.size() > idx && m_lodOfInstance[idx] == 3;
        const float th = wasFar ? sw.far * kFarLodHysteresis : sw.far;
        if (dist > th) return 3;
    }
    if (mesh.loIndexCount > 0 && sw.lo > 0.0f) {
        const bool wasLo = m_loLodActive.size() > idx && m_loLodActive[idx];
        const float th = wasLo ? sw.lo * kLoLodHysteresis : sw.lo;
        if (dist > th) return 2;
    }
    return 1;
}

void ModelRenderer::sortVisibleByMesh(std::vector<uint32_t>& list, bool useLod, bool recordStats) {
    // Agrupa por (MALHA, NIVEL DE LOD). Duas razoes:
    //
    // 1. O instancing EXIGE agrupamento - um draw instanciado so' cobre
    //    instancias consecutivas que compartilham buffers. Antes o default era
    //    desligado (ERUPTION_DRAW_SORT), e o instancing dependia da ordem
    //    acidental de carga.
    // 2. Agrupar por LOD tambem e' o que permite LIGAR o LOD sem perder o
    //    instancing. Medido: o GBuffer do parana_field e' 4,63 ms de GEOMETRIA
    //    + 0,46 ms/Mpx de pixel (83% geometria a 1080p) e Shadows e' 5,43 ms
    //    identico em 720p e 1080p (100% geometria). LOD e' a alavanca certa -
    //    mas com a lista agrupada so' por malha, cada vizinho em LOD diferente
    //    quebrava o run.
    //
    // O vetor m_lodOfInstance e' preenchido aqui e consumido pelo laco de
    // desenho IMEDIATAMENTE a seguir, na mesma chamada - passes diferentes
    // (cena, minimapa, sombra) nao se atrapalham porque cada um faz
    // sort-entao-desenha antes do proximo comecar.
    static const bool kOff = [] {
        const char* e = std::getenv("ERUPTION_DRAW_SORT");
        return e && std::string(e) == "0";
    }();
    const size_t n = list.size();
    if (kOff || n < 2 || m_meshes.empty()) return;

    const uint32_t meshCount = static_cast<uint32_t>(m_meshes.size());
    const size_t buckets = size_t(meshCount + 1) * kLodLevels;
    m_meshHistogram.assign(buckets + 1, 0);
    if (m_lodOfInstance.size() != m_instances.size()) m_lodOfInstance.assign(m_instances.size(), 1);

    for (uint32_t idx : list) {
        uint32_t m = m_instanceHot[idx].meshIndex;
        if (m >= meshCount) m = meshCount;
        if (useLod && m < meshCount) advanceLodDistance(idx, m_instanceHot[idx], m_meshes[m]);
        const uint8_t lod = (useLod && m < meshCount) ? lodLevelFor(idx, m_meshes[m]) : 1;
        if (useLod && idx < m_lodOfInstance.size()) m_lodOfInstance[idx] = lod;
        ++m_meshHistogram[size_t(m) * kLodLevels + lod + 1];
    }
    for (size_t i = 1; i <= buckets; ++i) m_meshHistogram[i] += m_meshHistogram[i - 1];

    // "Cache hit/miss" do agrupamento (F3): cada bucket (malha,LOD) NAO-VAZIO
    // vira UM draw/bind (o custo de uma troca de estado - a analogia com
    // cache-miss e' literal: e' a mesma nocao de "vizinho reusa o que ja' foi
    // buscado" que motivou a investigacao de SoA vs AoS). "Hit" = instancia
    // que caiu no MESMO bucket do run anterior, ou seja, nao paga bind novo.
    uint32_t runs = 0;
    for (size_t i = 1; i <= buckets; ++i) {
        if (m_meshHistogram[i] != m_meshHistogram[i - 1]) ++runs;
    }
    if (recordStats) {
        m_lastCacheMisses = runs;
        m_lastCacheHits = (n > runs) ? static_cast<uint32_t>(n) - runs : 0;
    }

    m_sortScratch.resize(n);
    for (uint32_t idx : list) {
        uint32_t m = m_instanceHot[idx].meshIndex;
        if (m >= meshCount) m = meshCount;
        const uint8_t lod = (useLod && idx < m_lodOfInstance.size()) ? m_lodOfInstance[idx] : 1;
        m_sortScratch[m_meshHistogram[size_t(m) * kLodLevels + lod]++] = idx;
    }

    // FRENTE-PARA-TRAS ENTRE BALDES (early-Z). O counting sort acima agrupa
    // por (malha, LOD), que e' o que o instancing exige, mas a ORDEM dos
    // baldes era o indice da malha - ou seja, arbitraria em relacao a camera.
    // Com isso o rasterizador desenha fundo antes de frente e o teste de
    // profundidade nao descarta nada: paga-se o fragment shader de pixels que
    // vao ser cobertos depois. Ordenar os BALDES pela distancia da instancia
    // mais proxima nao quebra um unico run (o balde continua contiguo), nao
    // muda o numero de draws, e da' ao early-Z a chance de rejeitar.
    // Referencia: de Lucas et al., "Visibility Rendering Order: Improving
    // Energy Efficiency on Mobile GPUs through Frame Coherence" (IEEE TC
    // 2018) - reordenar front-to-back rendeu 27% la'; e Anglada et al.,
    // "Early Visibility Resolution" (HPCA 2019).
    // DESLIGADO POR PADRAO (ERUPTION_DRAW_SORT_DEPTH=1 liga). Medido em
    // parana_field/1080p: rende so' 2-3% no GBuffer E MUDA A IMAGEM (RMS 10,85
    // com clima determinado, camera fixa). Para geometria opaca a ordem nao
    // deveria alterar nada; altera porque o passe usa DESCARTE POR ALFA
    // (`discard` no model.frag) - o que tambem explica por que o GBuffer nao
    // cai quando se corta triangulo: com discard a GPU nao pode rejeitar antes
    // de rodar o fragment shader, entao o early-Z que esta ordenacao pretendia
    // alimentar esta' desabilitado de qualquer forma. O caminho certo aqui e'
    // um depth pre-pass ou trocar discard por alpha-to-coverage, nao reordenar.
    static const bool kDepthOrder = [] {
        const char* e = std::getenv("ERUPTION_DRAW_SORT_DEPTH");
        return e && std::string(e) == "1";
    }();
    if (kDepthOrder && n > 1) {
        // Um registro por balde nao-vazio: (distancia minima, inicio, fim).
        m_bucketOrder.clear();
        for (size_t b = 0; b < buckets; ++b) {
            const uint32_t lo = (b == 0) ? 0u : m_meshHistogram[b];
            const uint32_t hi = m_meshHistogram[b + 1];
            if (hi <= lo) continue;
            float best = FLT_MAX;
            // Amostra no maximo 8 instancias do balde: a distancia so' precisa
            // ordenar, e varrer milhares de instancias por balde custaria mais
            // que o early-Z economiza.
            const uint32_t step = std::max(1u, (hi - lo) / 8u);
            for (uint32_t i = lo; i < hi; i += step) {
                const ModelInstanceHot& h = m_instanceHot[m_sortScratch[i]];
                const Vec3 c = (h.worldAabbMin + h.worldAabbMax) * 0.5f;
                { const Vec3 dv = c - m_lastCameraPos; best = std::min(best, glm::dot(dv, dv)); }
            }
            m_bucketOrder.push_back({best, lo, hi});
        }
        std::sort(m_bucketOrder.begin(), m_bucketOrder.end(),
                  [](const BucketRange& a, const BucketRange& b) { return a.dist2 < b.dist2; });
        list.clear();
        list.reserve(n);
        for (const BucketRange& br : m_bucketOrder)
            list.insert(list.end(), m_sortScratch.begin() + br.first,
                        m_sortScratch.begin() + br.last);
        return;
    }
    list.swap(m_sortScratch);
}

} // namespace eruption
