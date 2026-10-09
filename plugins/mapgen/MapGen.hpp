#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Gerador procedural de mapas (plugin do editor; não vai no jogo exportado).
//
// Uma receita (assets/mapgen/<nome>.mapgen.luau) é uma pilha de etapas que
// roda de cima para baixo sobre um campo de altura e produz o mapa direto no
// formato do motor: chão em grade (.glb) + grade de altura (.ter), camadas de
// pintura, objetos espalhados, água, início do jogador e grade de caminhada.
//
//   return {
//       size = 1000, seed = 7, origin = {0, 0},
//       steps = {
//           { "noise", scale = 320, height = 90 },
//           { "erosion", drops = 60000 },
//           { "paint", layer = "assets/textures/rock.png", slope_min = 28 },
//           { "scatter", model = "assets/props/tree.glb", density = 1.5, slope_max = 22 },
//           { "water", level = 8 },
//       },
//   }
namespace mapgen {

struct Step {
    std::string type;
    bool enabled = true;
    std::map<std::string, double> num;
    std::map<std::string, std::string> str;
    double get(const std::string& k, double def) const {
        auto it = num.find(k);
        return it == num.end() ? def : it->second;
    }
    std::string text(const std::string& k) const {
        auto it = str.find(k);
        return it == str.end() ? std::string() : it->second;
    }
};

struct Recipe {
    float size = 1000.0f; // lado do mapa em unidades do mundo
    float cell = 8.0f;    // distância entre pontos da grade
    uint32_t seed = 1;
    // Canto do mapa no mundo: dois mapas com a mesma receita e semente, um ao
    // lado do outro (origin = {1000, 0}), têm o relevo contínuo na emenda.
    float originX = 0.0f, originZ = 0.0f;
    std::vector<Step> steps;
    bool load(const std::string& path, std::string& error);
    bool parse(const std::string& source, std::string& error);
    std::string serialize() const;
};

// Parâmetros de cada tipo de etapa (para o editor mostrar e a IA saber).
struct ParamInfo { const char* name; double def, min, max; const char* help; };
struct StepInfo {
    const char* type;
    const char* label;
    const char* help;
    std::vector<ParamInfo> params;
    std::vector<const char*> texts; // parâmetros de texto (caminhos)
};
const std::vector<StepInfo>& stepCatalog();
const StepInfo* findStep(const std::string& type);

struct Layer {
    std::string texture;
    std::vector<float> weight; // um por ponto da grade (0..1)
};
struct ScatterGroup {
    std::string model; // .glb ou prefab
    struct Item { float x, y, z, yaw, scale; };
    std::vector<Item> items;
};

struct Result {
    int w = 0, d = 0;
    float cell = 8.0f;
    std::vector<float> height; // w * d, linha a linha em z
    std::vector<Layer> layers;
    std::vector<ScatterGroup> scatter;
    bool hasWater = false;
    float waterLevel = 0.0f;
    bool hasSpawn = false;
    float spawn[3] = {0, 0, 0};
    float maxWalkSlope = -1.0f; // < 0: tudo caminhável
    float originX = 0.0f, originZ = 0.0f; // mundo do ponto (0, 0) da grade
    std::vector<uint8_t> reserved;        // leito de rio e estrada: objetos não caem ali
    float h(int x, int z) const { return height[size_t(z) * w + x]; }
    float slopeDeg(int x, int z) const;
};

// Roda a receita. `resolution` > 0 força a grade (prévia rápida); 0 usa
// size/cell da receita. `progress` recebe 0..1 e pode devolver false para parar.
bool run(const Recipe& recipe, int resolution, Result& out, std::string& error,
         const std::function<bool(float, const char*)>& progress = {});

// Grava o mapa: assets/external/<nome>/<nome>.{glb,ter,env} e, ao lado da cena,
// as camadas, os objetos, a água, o início e a grade de caminhada.
bool writeMap(const Result& result, const std::string& projectRoot, const std::string& name, std::string& error);

} // namespace mapgen
