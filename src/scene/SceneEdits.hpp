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
//       added = {
//           { file = "assets/props/barril.glb", name = "barril_1", position = {...}, rotation = {...}, scale = {...} },
//           { prefab = "assets/prefabs/barril.prefab.luau", name = "barril_2", position = {...} },
//       },
//       copies = {
//           { from = "nome_do_objeto", name = "nome_novo", position = {...}, rotation = {...}, scale = {...} },
//       },
//       lights = { [3] = { position = {...}, color = {r, g, b}, intensity = 2, range = 40, enabled = true } },
//       characters = {
//           { name = "guarda", sprite = "assets/sprites/guarda.png", position = {...}, facing = 90 },
//       },
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
    // Modelo acrescentado ao mapa (arrastado do painel Projeto). Um .glb com
    // vários nós vira uma instância por nó, chamadas "<name>/<nó>".
    // `node`: só um nó do arquivo. `prefab`: o modelo, o nó, a rotação e a
    // escala vêm do prefab (mudar o prefab muda todas as instâncias).
    struct Added {
        std::string file, node, prefab, name;
        Transform transform;
    };
    // O que o motor criou para um Added: instâncias [first, first+count) e a
    // matriz de cada nó relativa à raiz do modelo.
    struct AddedInstance {
        std::string file, node, prefab, name;
        uint32_t first = 0, count = 0;
        std::vector<Mat4> local;
        Mat4 inner{1.0f}; // rotação/escala do prefab, já dentro de `local`
    };
    // Cria o modelo com o centro da base na origem da raiz:
    // instância = root * inner * (nó centrado). `inner` = rotação/escala do prefab.
    static bool addModel(Engine& engine, const std::string& file, const std::string& node, const std::string& name,
                         const Mat4& root, const Mat4& inner, AddedInstance& out);

    struct Light {
        int index = 0; // 1..
        Vec3 position{0.0f}, color{1.0f};
        float intensity = 1.0f, range = 10.0f;
        bool enabled = true;
    };

    std::vector<Added> added;
    std::vector<Object> objects;
    std::vector<Copy> copies;
    std::vector<Light> lights;
    // Personagem (NPC): sprite do importador, posição e para onde olha (graus).
    struct Character {
        std::string name, sprite;
        Vec3 position{0.0f};
        float facing = 0.0f;
        bool hidden = false; // começa escondido (um script mostra depois)
    };
    std::vector<Character> characters;
    bool hasEnvironment = false;
    float time = 12.0f;
    bool paused = false;
    std::string weather;

    bool empty() const {
        return added.empty() && objects.empty() && copies.empty() && lights.empty() && characters.empty() &&
               !hasEnvironment;
    }

    static std::filesystem::path pathFor(const std::string& map);
    // Falso se o arquivo existe mas não pôde ser lido (mensagem em `error`).
    bool load(const std::filesystem::path& file, std::string& error);
    std::string serialize(const std::string& map) const;
    // Devolve quantas edições foram aplicadas; nomes que não existem no mapa
    // são ignorados (o mapa pode ter sido gerado de novo).
    // Os modelos acrescentados entram primeiro, para que as outras edições
    // possam se referir a eles pelo nome; `addedOut` recebe o que foi criado.
    int apply(Engine& engine, std::vector<AddedInstance>* addedOut = nullptr) const;
};

// Prefab: um modelo (ou um nó dele) com rotação e escala, para soltar na cena
// quantas vezes quiser. assets/prefabs/<nome>.prefab.luau:
//
//   return { file = "assets/props/barril.glb", node = "Barril", rotation = {0, 30, 0}, scale = {2, 2, 2} }
struct Prefab {
    std::string file, node;
    Vec3 rotation{0.0f}, scale{1.0f};
    Mat4 matrix() const;
    bool load(const std::filesystem::path& path, std::string& error);
    std::string serialize() const;
    static bool isPrefabPath(const std::filesystem::path& p);
};

} // namespace eruption
