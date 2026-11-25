#pragma once

#include "math/Types.hpp"
#include "renderer/vfx/TornadoShape.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace eruption {

// Tornado (vfx.tornado do Luau). O funil, a saia de poeira e a nuvem-mae sao UM
// ray-march de densidade analitica (shaders/clouds/tornado.frag, desenhado por
// CloudLayerRenderer::renderTornado); aqui fica o estado de cada tornado (posicao,
// relogio, fade) e os destrocos, que sao sprites do ImmediateDraw em espiral pelo
// mesmo eixo e raio que o shader usa (TornadoShape.hpp). Sem script, nada aqui custa.
class ImmediateDraw;

// Tipo dos destrocos em espiral: o motor e' generico, o jogo escolhe o material.
enum class TornadoDebrisKind : int { Debris = 0, Leaves = 1, Dust = 2, Snow = 3 };

struct TornadoSpec {
    Vec3 pos = Vec3(0.0f);          // pe do tornado, em coordenadas de mundo (y = chao)
    float height = 200.0f;          // do chao ate' a base da nuvem-mae
    float radiusTop = 36.0f;        // raio da parede sob a nuvem
    float radiusBottom = 6.0f;      // raio da corda no chao
    float spin = 1.0f;              // 1 = rotacao tipica; escala a velocidade angular
    Vec3 dust = Vec3(0.46f, 0.38f, 0.29f); // cor da poeira do chao
    bool debris = true;             // pedras, folhas e tabuas em espiral
    float life = 0.0f;              // s ate' comecar a sumir; <= 0 fica ate' tornado_stop
    float fade = 3.0f;              // s da saida (a corda sobe e a poeira baixa)
    // Variacoes (o padrao e' o tornado de tempestade):
    float grow = 6.0f;              // s para a corda descer ate' o chao
    float cloud = 1.0f;             // escala da nuvem-mae; 0 = sem nuvem
    Vec3 color = Vec3(0.34f, 0.38f, 0.47f);      // cor do funil (condensacao)
    Vec3 cloudColor = Vec3(0.30f, 0.31f, 0.36f); // cor da nuvem-mae
    float density = 1.0f;           // multiplica a densidade (espessura)
    float opacity = 1.0f;           // multiplica o alfa final
    float wind = 1.0f;              // multiplica a rajada de vento em volta
    TornadoDebrisKind debrisKind = TornadoDebrisKind::Debris;
    float debrisSize = 1.0f;        // multiplica o tamanho dos destrocos
    float snowfall = 0.0f;          // 0..1: neve fina caindo em volta (flocos baratos); 0 = nenhuma
    std::string trailKind = "scorch"; // world.ground_state deixado no rastro; "" = nenhum
    float trailStrength = 0.30f;
};

class TornadoField {
public:
    struct Tornado {
        uint32_t id = 0;
        TornadoSpec spec;
        float age = 0.0f;           // relogio proprio (s); o shader usa o mesmo
        float endAge = -1.0f;       // idade em que a saida comecou (< 0 = ainda forte)
        float seed = 0.0f;          // radianos
        Vec3 lastTrail = Vec3(0.0f); // onde o rastro no chao foi deixado pela ultima vez
        float trailTimer = 0.0f;
        float burstTimer = 0.0f;
    };

    static constexpr size_t kMaxTornados = 8;
    static constexpr uint32_t kSnowfallMax = 100; // flocos da neve em volta, por tornado

    bool init(ImmediateDraw& draw);
    // Devolve o id (> 0) ou 0 se o teto de tornados simultaneos foi atingido.
    uint32_t start(const TornadoSpec& spec, size_t maxCount = kMaxTornados);
    bool move(uint32_t id, const Vec3& pos);
    // Comeca a saida (fade) agora; false se o id nao vive mais.
    bool stop(uint32_t id);
    void clear() { m_list.clear(); }
    void update(float dt);

    // 0..1: 1 forte, cai a 0 durante a saida. O tornado some da lista em 0.
    static float intensity(const Tornado& t);
    // 0..1: a corda desce da nuvem ate' o chao nos primeiros segundos.
    static float touchdown(const Tornado& t);
    static tornado_shape::Shape shapeOf(const Tornado& t);

    std::vector<Tornado>& all() { return m_list; }
    const std::vector<Tornado>& all() const { return m_list; }
    bool active() const { return !m_list.empty(); }

    // Destrocos de todos os tornados (no maximo `maxPieces` ao todo), como sprites do
    // quadro. `lit`: nivel de luz da cena (0..1) que escurece o material.
    void submitDebris(ImmediateDraw& draw, uint32_t maxPieces, float lit);
    uint32_t lastPieces() const { return m_lastPieces; }

    // Posicao de um destroco (testes e submitDebris): `i` indice, `cycle` o ciclo atual.
    struct Piece {
        Vec3 pos;
        float size = 1.0f;
        float rot = 0.0f;
        float alpha = 1.0f;
        float height01 = 0.0f;  // altura relativa (cor: poeira embaixo)
        int kind = 0;           // 0 pedra, 1 folha, 2 tabua, 3 floco/ponto macio, 4 cristal
    };
    static Piece pieceAt(const Tornado& t, uint32_t i);

private:
    std::vector<Tornado> m_list;
    uint32_t m_nextId = 1;
    int m_tex[5] = {-1, -1, -1, -1, -1};
    uint32_t m_lastPieces = 0;
};

} // namespace eruption
