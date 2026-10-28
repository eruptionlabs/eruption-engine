#pragma once

#include "formats/GltfParser.hpp"
#include "formats/TerrainParser.hpp"
#include "formats/NavGridTypes.hpp"
#include "formats/SmokeEmitter.hpp"
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace eruption {

// Forward declarations (GPU types not yet implemented)
class VulkanContext;
class BindlessDescriptor;

// Nevoa autoral (bloco "fog" do .env, ou world.set_fog). `set` falso = nao ha'.
struct MapFog {
    bool set = false;
    Vec3 color = Vec3(0.5f);
    float start = 50.0f, end = 1000.0f, opacity = 0.5f; // distancia em unidades do mundo
    float height = 0.0f, heightFalloff = 0.0f;          // nevoa de altura (0 = sem queda)
};
// Bloco "fog" ({color, start, end, opacity, height, falloff}); o mesmo formato
// serve ao world.set_fog.
MapFog parseMapFog(const nlohmann::json& f);

struct LoadedMap {
    std::string mapName;

    // Single-file GLB maps bring their own ground geometry and models.
    // The generated flat terrain is kept only as a hidden collision/walkable fallback.
    bool isGlbMap = true;

    std::vector<ModelFile> models;
    std::vector<TerrainMesh> terrainChunks;

    // Walkable surface generated for GLB maps.
    TerrainFile terrain;
    // true quando `terrain` veio do .ter do mapa; falso = grade de reserva
    // gerada (abaixo da geometria), que não serve para altura do chão.
    bool terrainFromFile = false;
    NavGridFile navGrid;
    // Custo de andar por célula de navGrid escrito pelo jogo, só nesta partida
    // (Engine::setWalkCost): vazio = 1 em todas; infinito = bloqueada.
    std::vector<float> walkCost;
    // Regras de terreno (Engine::setNavRules): 1 = célula íngreme. Refeita
    // quando navRulesVersion difere da do motor.
    std::vector<uint8_t> navSteep;
    uint32_t navRulesVersion = 0;

    // Lights extracted from the GLB (if any) or defaults.
    struct MapLight {
        Vec3 position;
        Vec3 color;
        float intensity = 1.0f;
        float range;
        LightAnimType animType = LightAnimType::Static;
        // Lamp/lantern props detected by material name: lit only at night.
        bool nightOnly = false;
        Vec3 spotDir = Vec3(0.0f); // .env "spotDir": [x,y,z]; zero = pontual
        float spotAngle = 0.0f;    // .env "spotAngle": abertura total em graus
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
        // Lava: calor extra perto da margem e no centro (0 = uniforme). Fenda
        // mais larga e quente nessas bordas; o meio-termo fica mais placa fria.
        float shoreHeat = 0.0f, centerHeat = 0.0f;
        float flowSpeed = 1.0f;
        // Lava: fracao da superficie coberta de crosta (0..1; < 0 = a do
        // shader, ~0,75) e tinta que multiplica a cor do basalto.
        float crustCoverage = -1.0f;
        Vec3 crustTint = Vec3(1.0f);
        // Margem LOBADA (opcional, nova feature): amostras de raio por
        // angulo, indice 0 = angulo 0, ultimo indice = quase 2*PI (o
        // wraparound entre o ultimo e o primeiro fecha o circulo). Uma
        // caldeira real nao e' um circulo - o raio onde a parede cruza a
        // cota do liquido varia por azimute (medido: 116 a 234 num vulcao
        // lobado). Um disco circular nao tem raio nenhum que cubra isso sem
        // sobrar vazio de um lado ou vazar pra fora de outro. Vazio = usa
        // `radius` pra todo angulo (mapas anteriores a essa feature).
        std::vector<float> shoreRadius;
        // Contorno LIVRE (opcional, chave "polygon"/"polygons" do .env):
        // poligonos no plano XZ, cada ponto com a propria altura (x, y, z) -
        // um rio que desce e' um poligono so', mais alto numa ponta. Quando ha'
        // contorno ele manda: centro, raio e shoreRadius ficam so' como
        // resumo (luz, world.liquids). `triangles` e' a triangulacao dele,
        // 3 pontos por triangulo, feita na carga (malha e consultas de chao).
        // `contourLevel` e' o `level` com que o contorno foi autorado: o nivel
        // vivo (setLiquidLevel) sobe ou desce o contorno inteiro pela diferenca.
        std::vector<std::vector<Vec3>> polygons;
        std::vector<Vec3> triangles;
        float contourLevel = 0.0f;
        // Malha do contorno pronta (triangulos subdivididos, UV x = calor de
        // margem), na altura do contorno, sem nudge nem giro: montar custava
        // 146 ms num contorno de 1135 pontos, e a lava e' remontada a cada
        // vizinho que entra ou sai. Quem desenha copia e desloca.
        std::shared_ptr<const WaterMesh> contourMesh;
    };
    std::vector<MapLiquid> liquids;
    // Triangula os poligonos de um liquido (orelhas no plano XZ) e resume
    // centro, raio e nivel a partir deles. Sem poligono nao faz nada.
    static void buildLiquidContour(MapLiquid& lq);
    // Monta lq.contourMesh a partir de `triangles` (buildLiquidContour chama).
    static void buildLiquidContourMesh(MapLiquid& lq);

    // AVENTAL DA BORDA ("edge_apron" no .env: true ou {first, bands, slope}).
    // Onde o mapa acaba sem vizinho, a camera baixa ve' ceu abaixo do chao.
    // O avental e' uma faixa de terreno que sai da borda da malha de chao para
    // fora em `bands` aneis (o primeiro com `first` u, cada um o dobro do
    // anterior), na altura da borda alisada ao longo do lado (mais longe, mais
    // alisada) e descendo `slope` u por u; leva o material e a cor da ultima
    // linha da grade. Um no' por lado (kEdgeApronPrefix + x0/x1/z0/z1), para
    // esconder o lado que tem vizinho (Engine::updateEdgeAprons). Sem a chave,
    // nada muda.
    struct EdgeApronParams {
        bool enabled = false;
        float first = 64.0f;
        int bands = 6;
        float slope = 0.03f;
    } edgeApronParams;
    static constexpr const char* kEdgeApronPrefix = "edge_apron_";
    // Pontos logo fora de cada lado (espaco do no'): coberto por chao de
    // outro mapa = lado escondido.
    struct EdgeApron {
        std::string node;
        Vec3 probe[3];
    };
    std::vector<EdgeApron> edgeAprons;
    // Acrescenta os nos do avental ao modelo do mapa (malha de chao = o no'
    // que cobre a maior area). Falso se nao achou chao em grade.
    bool buildEdgeAprons(ModelFile& model);

    // Emissores de fumaça declarados no .env (chave opcional "smoke").
    // Vazio = mapa sem fumaça (todo mapa antigo cai aqui).
    std::vector<SmokeEmitter> smokeEmitters;

    // Efeitos de particula do mapa (chave opcional "effects" do .env): cada
    // um toca o efeito `name` (assets/effects do jogo ou data/vfx do motor)
    // no lugar dado, desde a abertura do mapa.
    struct MapEffect {
        std::string name;
        Vec3 position{0.0f};
        Vec3 rotationDeg{0.0f}; // graus, em torno de X, Y e Z
        float scale = 1.0f;
        bool enabled = true;
        bool nightOnly = false; // como as luzes: so' toca de noite
    };
    std::vector<MapEffect> effects;
    // Unidades do mundo por metro para os efeitos (0 = padrao do motor).
    float vfxUnitsPerMeter = 0.0f;

    // Environment/lighting values loaded from the map's .env companion file.
    struct {
        // NEVOA DO MAPA ("fog" no .env): quando presente, vence a do clima e a
        // cor do ceu do ciclo dia/noite - um interior (caverna, sala) tem a
        // propria bruma, que nao muda com chuva la' fora nem com a hora.
        MapFog fog;
        // FUNDO ("background" no .env): o que aparece onde nenhuma geometria
        // cobre o pixel e o raio aponta para baixo do horizonte (borda do
        // mundo). 0 = o ceu de sempre, 1 = a cor da nevoa, 2 = backgroundColor.
        // Fora do 0, esses pixels tambem saem da autoexposicao.
        int backgroundMode = 0;
        // EXPOSICAO TRAVADA ("exposure": {"auto": false, "value": 1.0}): o
        // multiplicador da autoexposicao fica nesse valor, sem histograma.
        // Para luz calibrada a mao contra uma referencia.
        bool exposureLocked = false;
        float exposureValue = 1.0f;
        Vec3 backgroundColor = Vec3(0.0f);
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
        // AMBIENTE AUTORAL. Sem esta marca o `ambient` do .env era lido,
        // guardado aqui e perdido no quadro seguinte: Render.cpp reescreve o
        // ambiente inteiro a partir do ciclo dia/noite. Num mapa de INTERIOR
        // (caverna, tubo de lava) nao existe hora do dia que preste, e o autor
        // ficava sem nenhum controle de preenchimento - girando um botao
        // desligado. Marcado, o valor do mapa vence o ciclo.
        bool ambientAuthored = false;
        // O .env PEDIU para mandar na luz: "light": { "authoritative": true }.
        //
        // Tem que ser pedido explicito, nao a simples presenca do bloco
        // `light`: os ~1040 mapas convertidos em assets/external todos tem
        // esse bloco gravado pelo conversor (o .env de cada um traz ambient,
        // diffuse, sunDirection e opacity). Ligar pela presenca congelava o
        // sol de todos eles e matava o ciclo dia/noite - um mapa ficava de
        // noite com o chao aceso de dia.
        //
        // Quem precisa disto e' mapa de INTERIOR, onde nao existe hora do
        // dia: ali o autor liga a chave e passa a mandar no sol e no
        // ambiente. A ceu aberto o ciclo continua no comando, como sempre.
        bool lightAuthored = false;
        // INTERIOR FECHADO: "sky": false no .env tira o ceu do fundo.
        //
        // Um tubo de lava nao tem ceu, mas o motor desenhava a cupula do
        // mesmo jeito: bastava a camera subir um pouco para aparecer uma
        // faixa roxa de ceu noturno atras da rocha, e a caverna virava um
        // vale a ceu aberto. Desligado, o fundo fica na cor de limpeza e o
        // que nao tem geometria le' como escuro.
        bool hasSky = true;
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
