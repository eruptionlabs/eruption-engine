#pragma once

#include "math/Types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace eruption {

class Engine;

// Edições feitas sobre um mapa (no editor ou por uma IA), guardadas num
// arquivo Luau ao lado dos assets: assets/scenes/<mapa>.scene.luau.
//
//   return {
//       objects = {
//           ["nome_do_objeto"] = { position = {x, y, z}, rotation = {x, y, z}, scale = {x, y, z} },
//           ["outro_objeto"] = { hidden = true },
//       },
//       copies = {
//           { from = "nome_do_objeto", name = "nome_novo", position = {...}, rotation = {...}, scale = {...} },
//       },
//       lights = { [3] = { position = {...}, color = {r, g, b}, intensity = 2, range = 40, enabled = true } },
//       environment = { time = 18.5, paused = true, weather = "rainy" },
//   }
//
// Rotação em graus. O jogo aplica o arquivo ao carregar o mapa, então o que
// se arruma no editor é o que o jogador vê.
struct SceneEdits {
    struct Transform {
        Vec3 position{0.0f}, rotation{0.0f}, scale{1.0f};
        Mat4 matrix() const;
        static Transform from(const Mat4& m);
    };
    struct Object {
        std::string name;
        bool hasTransform = false;
        Transform transform;
        bool hidden = false;
    };
    struct Copy {
        std::string from, name;
        Transform transform;
    };
    struct Light {
        int index = 0; // 1..
        Vec3 position{0.0f}, color{1.0f};
        float intensity = 1.0f, range = 10.0f;
        bool enabled = true;
    };

    std::vector<Object> objects;
    std::vector<Copy> copies;
    std::vector<Light> lights;
    bool hasEnvironment = false;
    float time = 12.0f;
    bool paused = false;
    std::string weather;

    bool empty() const { return objects.empty() && copies.empty() && lights.empty() && !hasEnvironment; }

    static std::filesystem::path pathFor(const std::string& map);
    // Falso se o arquivo existe mas não pôde ser lido (mensagem em `error`).
    bool load(const std::filesystem::path& file, std::string& error);
    std::string serialize(const std::string& map) const;
    // Devolve quantas edições foram aplicadas; nomes que não existem no mapa
    // são ignorados (o mapa pode ter sido gerado de novo).
    int apply(Engine& engine) const;
};

} // namespace eruption
