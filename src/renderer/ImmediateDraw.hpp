#pragma once

#include "renderer/VulkanContext.hpp"
#include "math/Types.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace eruption {

// Desenho imediato vindo de script (world.draw_strip, world.draw_sprite): o
// script manda os vertices a cada quadro, o passe desenha e esquece. Generico:
// nao sabe o que e' um portal, so' strips/quads/billboards texturizados.
//
// Custo: um anel de vertices/indices por quadro em voo (mapeado, sem alocar no
// quadro); as chamadas sao agrupadas por (textura, blend, flags) para poucos
// draws; um pipeline por combinacao de blend/depth_test/cull, criado na primeira
// vez que aparece.
class ImmediateDraw {
public:
    enum class Blend : uint8_t { Additive = 0, Alpha = 1 };
    enum class Topology : uint8_t { Strip = 0, Quads = 1 };

    // 48 bytes, tudo float32 (nenhum formato de 16 bits: regra dos celulares fracos).
    // A normal so' conta no fresnel do material; zero = usa a da face.
    struct Vertex {
        float x, y, z;
        float u, v;
        float r, g, b, a;
        float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    };

    // Material opcional de um comando (so' mexe no alfa; o padrao e' o desenho
    // de sempre). Fresnel pela normal, erosao por um ruido rolando em UV e
    // fade suave contra a profundidade da cena.
    struct Material {
        bool fresnel = false, invert = false;
        float fresnelPower = 2.0f;
        int noiseTex = -1;                 // textura de ruido (canal R); >= 0 liga a erosao
        float scrollU = 0.0f, scrollV = 0.0f; // UV por segundo
        float tileU = 1.0f, tileV = 1.0f;
        float erode = 0.0f, erodeSoft = 0.1f;
        float softDepth = 0.0f;            // > 0 liga o fade contra a cena (unidades do mundo)
        // Com luz: sol da cena (cor e direcao) e ambiente hemisferico pela normal da malha,
        // como o chao iluminado do deferred. Sem sombra e sem IBL/sondas (ver View::Light).
        bool lit = false;
        bool any() const { return fresnel || noiseTex >= 0 || softDepth > 0.0f || lit; }
        bool operator==(const Material& o) const {
            return fresnel == o.fresnel && invert == o.invert && fresnelPower == o.fresnelPower &&
                   noiseTex == o.noiseTex && scrollU == o.scrollU && scrollV == o.scrollV && tileU == o.tileU &&
                   tileV == o.tileV && erode == o.erode && erodeSoft == o.erodeSoft && softDepth == o.softDepth &&
                   lit == o.lit;
        }
    };

    struct Flags {
        Blend blend = Blend::Additive;
        bool depthTest = true;
        bool cull = false;
    };

    // Teto por quadro (indices de 16 bits). O que passar disso e' descartado,
    // com um aviso por execucao.
    static constexpr uint32_t kMaxVertices = 32768;
    static constexpr uint32_t kMaxIndices = kMaxVertices * 6; // malhas: ~2 triangulos por vertice
    static constexpr uint32_t kMaxTextures = 64;
    static constexpr uint32_t kMaxMeshVertices = 16384;
    static constexpr uint32_t kMaxMeshes = 32;

    bool init(VulkanContext* ctx, VkFormat colorFormat, VkFormat depthFormat);
    void shutdown();
    bool ready() const { return m_ready; }

    // Id da textura (PNG do projeto, em cache por caminho, carregado uma vez);
    // -1 se o arquivo nao abriu (o aviso sai uma vez por caminho).
    int textureId(const std::string& path);
    // Textura procedural (RGBA8, em cache pela chave): perfis suaves de feixe e
    // ponto dos efeitos do motor (raio, precipitacao). -1 se nao coube.
    int addTexture(const std::string& key, const uint8_t* rgba, int w, int h);

    // Reserva `count` vertices de uma strip/lista de quads e devolve onde
    // escreve-los (nulo se nao cabe no quadro). A chamada e' do tamanho certo
    // ou nada: nunca deixa a lista pela metade.
    Vertex* addGeometry(int tex, Topology topo, Flags flags, uint32_t count, const Material* mat = nullptr);
    // Malha .glb do projeto (cache por caminho, a primeira malha com triangulos;
    // teto de kMaxMeshVertices); -1 se nao abriu (um aviso por caminho).
    int meshId(const std::string& path);
    // Desenha a malha `mesh` com `xf` (modelo -> mundo) e a cor `tint` (HDR) a cada
    // quadro; a normal vai junto para o fresnel.
    void addMesh(int mesh, int tex, const Mat4& xf, const Vec4& tint, Flags flags, const Material* mat);
    // Billboard: `size` em unidades do mundo, ou em pixels da janela com
    // `pixels`. `rot` em radianos, no plano da tela.
    void addSprite(int tex, const Vec3& pos, float w, float h, const Vec4& rgba, Flags flags, bool pixels, float rot);

    bool hasWork() const { return !m_cmds.empty(); }

    struct View {
        VkImageView colorView = VK_NULL_HANDLE; // imagem iluminada (HDR, em COLOR_ATTACHMENT_OPTIMAL)
        VkImage colorImage = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE; // profundidade da cena (DEPTH_STENCIL_READ_ONLY_OPTIMAL)
        VkExtent2D extent{};
        Vec3 camPos{0.0f};        // fresnel
        Vec3 depthProj{0.0f};     // P[2][2], P[3][2], P[2][3] da projecao sem jitter (soft contra a cena)
        double time = 0.0;        // segundos, para o scroll do ruido
        Mat4 viewProj{1.0f};      // a mesma (com jitter) da geometria que escreveu a profundidade
        Vec3 camRight{1, 0, 0}, camUp{0, 1, 0};
        float proj11 = 1.0f;      // proj[1][1] sem jitter (cot(fov/2)), para tamanho em pixels
        float screenHeightPx = 1.0f;
        // Luz da cena para Material::lit (os mesmos numeros do FrameUBO que o deferred usa).
        struct Light {
            Vec4 sunDir{0.0f, -1.0f, 0.0f, 0.0f};  // xyz = para onde a luz viaja, w = intensidade
            Vec4 sunColor{1.0f};                   // rgb
            Vec4 ambientSky{0.0f};                 // rgb = ceu (zenite), w = intensidade do GI
            Vec4 ambientGround{0.0f};              // rgb = chao
        } light;
    };
    // Desenha tudo o que foi pedido e esvazia a lista. Chamar com o rendering
    // fechado. Devolve false se nao desenhou nada.
    bool render(VkCommandBuffer cmd, const View& view);
    // Esvazia sem desenhar (quadro sem passe de cena).
    void discard() { m_cmds.clear(); m_verts.clear(); m_sprites.clear(); m_idx.clear(); m_mats.clear(); m_pending = 0; }

    // Contagens do ultimo render() (telemetria/teste).
    uint32_t lastDraws() const { return m_lastDraws; }
    uint32_t lastVertices() const { return m_lastVertices; }

private:
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    struct Cmd {
        int tex;
        uint8_t flags;      // bit0 alpha, bit1 depthTest, bit2 cull
        uint8_t kind;       // 0 strip, 1 quads, 2 sprite, 3 malha indexada
        uint32_t first;     // indice em m_verts (geometria) ou m_sprites (sprite)
        uint32_t count;     // vertices (geometria)
        uint32_t mat = 0;   // 0 = sem material; senao 1 + indice em m_mats
        uint32_t idxFirst = 0, idxCount = 0; // malha: faixa em m_idx (indices locais ao comando)
    };
    struct MeshData {
        std::vector<Vertex> verts; // modelo (cor branca)
        std::vector<uint16_t> idx;
        // Cor base do material do .glb (RGBA8, no maximo 512 px); so' sobe para a GPU
        // no primeiro desenho com `lit` e sem textura pedida.
        std::vector<uint8_t> baseRgba;
        int baseW = 0, baseH = 0;
        int baseTex = -2; // -2 = ainda nao subiu, -1 = sem textura base
        std::string path;
    };
    struct SpriteCmd {
        Vec3 pos;
        float w, h, rot;
        Vec4 rgba;
        bool pixels;
    };
    struct FrameBuf {
        VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
        VmaAllocation vbAlloc = VK_NULL_HANDLE, ibAlloc = VK_NULL_HANDLE;
        void* vbMap = nullptr;
        void* ibMap = nullptr;
    };

    static uint8_t packFlags(Flags f) { return uint8_t((f.blend == Blend::Alpha ? 1 : 0) | (f.depthTest ? 2 : 0) | (f.cull ? 4 : 0)); }
    VkPipeline pipelineFor(uint8_t flags);
    uint32_t materialIndex(const Material* mat);
    bool loadMesh(const std::string& path, MeshData& out);
    void writeDepthSet(uint32_t frame, VkImageView depthView);
    bool loadTexture(const std::string& path, Texture& out);
    bool uploadTexture(const uint8_t* px, int w, int h, Texture& out);
    void destroyTexture(Texture& t);
    void warnOnce(const char* what);

    VulkanContext* m_ctx = nullptr;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED, m_depthFormat = VK_FORMAT_UNDEFINED;
    bool m_ready = false;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_depthLayout = VK_NULL_HANDLE; // set 2: profundidade da cena (sempre escrito)
    // Set 3: luz da cena (UBO de 64 B por quadro em voo), escrito a cada render()
    // mesmo sem nenhum desenho com luz. Nao cabe em push constant: o material ja' usa os 128 B.
    VkDescriptorSetLayout m_lightLayout = VK_NULL_HANDLE;
    VkBuffer m_lightBuf[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    VmaAllocation m_lightAlloc[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    void* m_lightMap[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    VkDescriptorSet m_lightSets[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    VkSampler m_depthSampler = VK_NULL_HANDLE;
    VkDescriptorSet m_depthSets[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    VkImageView m_depthSetView[VulkanContext::MAX_FRAMES_IN_FLIGHT] = {};
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkShaderModule m_vert = VK_NULL_HANDLE, m_frag = VK_NULL_HANDLE;
    VkPipeline m_pipes[8] = {};
    FrameBuf m_frames[VulkanContext::MAX_FRAMES_IN_FLIGHT];

    std::vector<Texture> m_textures;
    std::unordered_map<std::string, int> m_texIds;

    std::vector<Cmd> m_cmds;
    std::vector<Vertex> m_verts;
    std::vector<SpriteCmd> m_sprites;
    std::vector<uint16_t> m_idx;     // indices das malhas do quadro (locais a cada comando)
    std::vector<Material> m_mats;    // materiais distintos do quadro
    std::vector<MeshData> m_meshData;
    std::unordered_map<std::string, int> m_meshIds;
    uint32_t m_pending = 0; // vertices dos comandos da lista (sprites contam 4)
    bool m_warned = false;
    uint32_t m_lastDraws = 0, m_lastVertices = 0;
    std::vector<uint32_t> m_order; // scratch: ordem de desenho dos comandos
};

} // namespace eruption
