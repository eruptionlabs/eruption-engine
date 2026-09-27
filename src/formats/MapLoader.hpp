#pragma once

#include "formats/GltfParser.hpp"
#include "formats/TerrainParser.hpp"
#include "formats/NavGridTypes.hpp"
#include "formats/SmokeEmitter.hpp"
#include <memory>
#include <string>
#include <vector>

namespace eruption {

// Forward declarations (GPU types not yet implemented)
class VulkanContext;
class BindlessDescriptor;

struct LoadedMap {
    std::string mapName;

    // Single-file GLB maps bring their own ground geometry and models.
    // The generated flat terrain is kept only as a hidden collision/walkable fallback.
    bool isGlbMap = true;

    std::vector<ModelFile> models;
    std::vector<TerrainMesh> terrainChunks;

    // Walkable surface generated for GLB maps.
    TerrainFile terrain;
    NavGridFile navGrid;

    // Lights extracted from the GLB (if any) or defaults.
    struct MapLight {
        Vec3 position;
        Vec3 color;
        float intensity = 1.0f;
        float range;
        LightAnimType animType = LightAnimType::Static;
        // Lamp/lantern props detected by material name: lit only at night.
        bool nightOnly = false;
    };
    std::vector<MapLight> lights;

    // Water planes (defaults only for now)
    std::vector<TerrainWaterPlane> waterPlanes;

    struct MapLiquid {
        std::string name;
        int kind = 0;
        float level = 0.0f;
        float centerX = 0.0f;
        float centerZ = 0.0f;
        float radius = 0.0f;      // raio circular; usado quando shoreRadius esta' vazio
        float emissive = 0.0f;
        float flowSpeed = 1.0f;
        // Margem LOBADA (opcional, nova feature): amostras de raio por
        // angulo, indice 0 = angulo 0, ultimo indice = quase 2*PI (o
        // wraparound entre o ultimo e o primeiro fecha o circulo). Uma
        // caldeira real nao e' um circulo - o raio onde a parede cruza a
        // cota do liquido varia por azimute (medido: 116 a 234 num vulcao
        // lobado). Um disco circular nao tem raio nenhum que cubra isso sem
        // sobrar vazio de um lado ou vazar pra fora de outro. Vazio = usa
        // `radius` pra todo angulo (mapas anteriores a essa feature).
        std::vector<float> shoreRadius;
    };
    std::vector<MapLiquid> liquids;

    // Emissores de fumaça declarados no .env (chave opcional "smoke").
    // Vazio = mapa sem fumaça (todo mapa antigo cai aqui).
    std::vector<SmokeEmitter> smokeEmitters;

    // Environment/lighting values loaded from the map's .env companion file.
    struct {
        Vec3 sunDirection = Vec3(0.0f, -1.0f, 0.0f);
        Vec3 sunColor = Vec3(2.5f, 2.4f, 2.2f);
        float sunIntensity = 1.0f;
        Vec3 ambientColor = Vec3(2.0f, 2.0f, 2.1f);
        // Override da cor do quique (bounce/ambiente de chao), vindo do .env.
        // Sem ele a cor sai da MEDIA de todas as texturas embutidas do GLB -
        // arvore, folha, rocha, lava - e o resultado e' um cinza quase neutro,
        // quando o quique de um latossolo roxo devia ser laranja saturado.
        // Negativo = nao definido (usa a media automatica).
        Vec3 groundAlbedoOverride = Vec3(-1.0f);
        float ambientIntensity = 1.0f;
    } env;

    // Approximate CPU RAM used by this map's raw parsed data
    size_t rawDataSize = 0;
};

class MapLoader {
public:
    MapLoader() = default;

    std::shared_ptr<LoadedMap> loadMap(const std::string& mapName);

    bool beginLoad(const std::string& mapName);
    bool loadPhaseTextures();
    bool loadPhaseGeometry();
    bool loadPhaseModels();
    std::shared_ptr<LoadedMap> finalizeLoad();

private:
    bool loadFromGlb();
    void loadTerrainChunks();

    std::string m_loadingMapName;
    std::shared_ptr<LoadedMap> m_loadingMap;
    float m_loadProgress = 0.0f;
};

} // namespace eruption
