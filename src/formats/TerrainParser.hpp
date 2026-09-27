#pragma once

#include "utils/BinaryReader.hpp"
#include "math/Types.hpp"
#include <vector>
#include <string>

namespace eruption {

struct TerrainSurface {
    float u[4];
    float v[4];
    uint16_t textureId;
    uint16_t lightmapId;
    uint32_t color;
    // Total: 32 + 2 + 2 + 4 = 40 bytes (no padding in file format)
};

struct TerrainCube {
    float height[4];
    int32_t surfaceTop;
    int32_t surfaceNorth;
    int32_t surfaceEast;
};

struct TerrainWaterPlane {
    float level;
    uint32_t type;
    float waveHeight;
    float waveSpeed;
    float wavePitch;
    uint32_t textureCycling;
};

struct TerrainLightmapSlice {
    std::vector<uint8_t> shadowmap; // 64 bytes (8x8)
    std::vector<uint8_t> lightmap_rgb; // 192 bytes (8x8x3)
};

struct TerrainFile {
    uint16_t version = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    float scale = 10.0f;
    float offsetX = 0.0f;
    float offsetZ = 0.0f;

    std::vector<std::string> textures;
    std::vector<TerrainSurface> surfaces;
    std::vector<TerrainCube> cubes;
    std::vector<TerrainLightmapSlice> lightmapSlices;

    std::vector<TerrainWaterPlane> waterPlanes;
    bool hasWater() const { return !waterPlanes.empty(); }

    // Computed data
    std::vector<Vec3> smoothNormals;
    std::vector<uint32_t> smoothColors;
};

struct TerrainVertex {
    // LAYOUT EMPACOTADO (2026-09-12): 104 -> 88 bytes, SEM PERDA.
    //
    // O G-buffer e' limitado por FETCH de vertice (medido: forcar LOD base
    // multiplica invocacoes de vertice por 6,5x e o passe por 2,8x - 0,5 ns
    // por vertice, que e' banda de memoria, nao ALU). Vertice menor = menos
    // banda. Os nove indices de material/bindless cabem em 16 bits (slots
    // bindless < 4096, highWater 728 em San Miguel) e o shader continua
    // recebendo `uint` - VK_FORMAT_R16_UINT estende com zero na entrada, entao
    // NENHUM shader muda, so' o VkFormat dos atributos.
    //
    // O que NAO narrowou, de proposito: posicao (mundo ate' 2000+ u; half
    // perde 1,0 u em 2048), UV (tiling > 1 estoura a precisao de half),
    // normal (snorm16 nao e' bit-identico) e os floats de peso. O criterio
    // desta rodada e' imagem PIXEL A PIXEL IDENTICA; o que perde bit fica
    // para uma rodada com FLIP e decisao do autor.
    //
    // A ordem e' de alinhamento: os 12 campos de 4 B primeiro, os nove de
    // 2 B no fim (18 B + 2 de padding = 20). Ninguem inicializa isto por
    // posicao, e reindexMesh/LOD/cache tratam o vertice como bytes opacos.
    Vec3 position;
    Vec2 texCoord;
    Vec3 normal;
    uint32_t color;
    float blendWeight = 0.0f;
    // World-space splat mask (optional, new feature): when blendMaskIndex != 0
    // the fragment shader samples u_textures[blendMaskIndex] at blendMaskUV to
    // get a continuous, mesh-independent crossfade weight instead of the
    // per-vertex blendWeight above (which is affine-interpolated per triangle
    // and shows a seam along each quad's diagonal). Zero/absent on every GLB
    // that predates this feature, so existing maps render exactly as before.
    Vec2 blendMaskUV = Vec2(0.0f);
    // Splat de 4 camadas (feature nova). O blend de 2 materiais acima tem um
    // teto estrutural: um triangulo carrega UM material base e UM alvo, entao
    // onde o material base precisa mudar a mudanca cai na aresta do quad -
    // uma grade de 10u alinhada aos eixos, que na tela desenha retangulo e
    // triangulo por mais perfeita que a mascara seja.
    //
    // Com splat, TODA primitiva de terreno usa o mesmo material base e as
    // mesmas 4 camadas, e quem decide a mistura e' so' a mascara RGBA
    // amostrada em UV de MUNDO (continua por construcao). Nao sobra nada
    // per-quad, entao nao ha' onde aparecer aresta de malha.
    //
    // Empacotado como 2x uint32 (4 slots de 16 bits) pra nao gastar mais um
    // atributo de vertice nem 16 bytes em TODA malha do jogo - props
    // incluidos, que sao a maior parte dos vertices.
    // Zero nos dois = sem splat, e o caminho antigo roda igualzinho.
    uint32_t splatTex01 = 0;
    uint32_t splatTex23 = 0;
    // Segunda mascara RGBA (feature nova, dobra o splat de 4 pra 8 camadas +
    // base = 9). Mesma ideia do par acima: 2x uint32 = 4 slots de 16 bits, e
    // reusa a MESMA blendMaskUV (e' so' posicao de mundo, nao muda entre as
    // duas mascaras). blendMaskIndex2 == 0 = sem segunda mascara, e o
    // shader cai pro caminho de 5 camadas de sempre - todo GLB anterior a
    // essa feature (incluindo os que ja' tem o splat de 4) renderiza igual.
    uint32_t splatTex45 = 0;
    uint32_t splatTex67 = 0;
    // Optional self-lit glow (new feature, e.g. lava near a volcano vent).
    // 0 = no glow (every GLB baked before this feature), authored via the
    // _EMISSIVE_STRENGTH vertex attribute. The fragment shader multiplies it
    // by a fixed warm color; no texture lookup involved.
    float emissiveStrength = 0.0f;
    // --- 16 bits: indices bindless e de material (VK_FORMAT_R16_UINT) ---
    uint16_t texIndex = 0;
    uint16_t matId = 0;
    uint16_t pbrIndex = 0;
    uint16_t normalIndex = 0;
    uint16_t blendTexIndex = 0;
    uint16_t blendPbrIndex = 0;
    uint16_t blendNormalIndex = 0;
    uint16_t blendMaskIndex = 0;
    uint16_t blendMaskIndex2 = 0;
};
static_assert(sizeof(TerrainVertex) == 88, "TerrainVertex: layout empacotado e' 88 bytes - se mudou, revise os VkVertexInputAttributeDescription");


struct TerrainMesh {
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
};

struct WaterVertex {
    Vec3 position;
    Vec2 texCoord;
};

struct WaterMesh {
    std::vector<WaterVertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t waterTileCount = 0;
};

struct ExtractedLight {
    Vec3 worldPos;
    Vec3 color;
    float intensity;
    float radius;
    LightAnimType animType;
};

class TerrainParser {
public:
    static TerrainFile parse(const uint8_t* data, size_t size);
    static TerrainFile parse(const std::vector<uint8_t>& data) {
        return parse(data.data(), data.size());
    }

    static TerrainMesh generateMesh(const TerrainFile& terrain,
                                    uint32_t startX, uint32_t startZ,
                                    uint32_t chunkW, uint32_t chunkH);

    static WaterMesh generateWaterMesh(const TerrainFile& terrain,
                                       uint32_t startX, uint32_t startZ,
                                       uint32_t chunkW, uint32_t chunkH,
                                       float waterLevel, float waveHeight,
                                       bool forceAllTiles = false,
                                       bool skipIfPositive = true);

    static float getTerrainHeightAt(const TerrainFile& terrain, float worldX, float worldZ);

    static void computeSmoothNormals(TerrainFile& terrain);
    static void computeSmoothColors(TerrainFile& terrain);
    static std::vector<ExtractedLight> extractPointLights(const TerrainFile& terrain);
    static std::vector<ExtractedLight> extractLightProbes(const TerrainFile& terrain);
};

} // namespace eruption
