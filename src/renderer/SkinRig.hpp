#pragma once

// Esqueleto e animações de um .glb com skin (glTF 2.0): os nós do arquivo com
// a pose de repouso, as skins (ossos + matrizes de bind inversas) e as
// animações com nome. Só dados e amostragem na CPU; o skinning dos vértices é
// feito na GPU (ModelRenderer, shaders/compute/skin.comp).
#include "math/Types.hpp"

#include <string>
#include <vector>

namespace eruption {

struct SkinRig {
    struct Node {
        std::string name;
        int parent = -1;
        Vec3 t{0.0f};
        glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 s{1.0f};
    };
    struct Skin {
        std::vector<int> joints;          // nós do arquivo
        std::vector<glm::mat4> inverseBind;
    };
    struct Channel {
        int node = -1;
        int path = 0;       // 0 posição, 1 rotação, 2 escala
        bool step = false;  // STEP (senão LINEAR; CUBICSPLINE vira LINEAR nos valores)
        std::vector<float> times;
        std::vector<Vec4> values; // rotação xyzw; posição/escala xyz
    };
    struct Animation {
        std::string name;
        float duration = 0.0f;
        std::vector<Channel> channels;
    };

    std::vector<Node> nodes;
    std::vector<int> order; // pais antes dos filhos
    std::vector<Skin> skins;
    std::vector<Animation> animations;

    int findAnimation(const std::string& name) const;
    // Matrizes de mundo (espaço do modelo) de todos os nós na pose da animação
    // `anim` no tempo `time` (anim < 0: pose de repouso).
    void pose(int anim, float time, std::vector<glm::mat4>& globals) const;
    // Transição entre duas animações: `from` no tempo `fromTime` com peso
    // 1 - w e `anim` no tempo `time` com peso w (por nó: posição e escala
    // interpoladas, rotação por slerp).
    void blendPose(int from, float fromTime, int anim, float time, float w, std::vector<glm::mat4>& globals) const;
    // Matrizes dos ossos da skin para o shader: global do osso * bind inversa.
    void jointMatrices(int skin, const std::vector<glm::mat4>& globals, glm::mat4* out) const;
};

} // namespace eruption
