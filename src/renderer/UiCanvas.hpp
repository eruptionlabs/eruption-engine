#pragma once

// EFEITOS 2D DE JOGO — o que tem que ser exato no frame e sobreviver ao
// script que pediu.
//
// A INTERFACE do jogo (menu, fila, ficha) mora em renderer/UiDocument: vem
// de um arquivo CSS, editável no Caldera. Aqui ficam só as duas coisas que
// não cabem num arquivo de layout:
//
//   * o NÚMERO que sobe de um ponto do MUNDO (dano, cura, "ERROU") — precisa
//     da projeção da câmera, e vive sozinho depois de pedido;
//   * o CLARÃO / FADE de tela cheia — a transição de batalha depende de ele
//     ser exato no frame.
//
// Coordenadas em fração da área de jogo (0..1), para valer igual em qualquer
// resolução e dentro do painel Cena do editor. Tamanho de texto em fração da
// ALTURA da área (0.04 ≈ 4% da tela).

#include "math/Types.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct ImDrawList;

namespace eruption {

class UiCanvas {
public:
    // Número que sobe e desaparece, ancorado numa posição de MUNDO (dano,
    // cura, "MISS"). O motor projeta e segura o tempo de vida.
    void popup(const Vec3& world, const std::string& text, const Vec4& color, float seconds, float rise, float size);
    // Clarão/fade de tela cheia: a cor vai de `from` a `to` de opacidade.
    void screenFade(const Vec4& color, float from, float to, float seconds);
    bool fading() const { return m_fade.left > 0.0f; }

    // Uma vez por frame, antes de desenhar.
    void update(float dt);
    // Desenha na área (x, y, w, h) em pixels de tela. `viewProj` projeta os
    // números de mundo; passar a matriz sem jitter.
    void draw(ImDrawList* dl, float x, float y, float w, float h, const Mat4& viewProj) const;

private:
    struct Popup {
        Vec3 world{0.0f};
        std::string text;
        Vec4 color{1.0f};
        float left = 0.0f, total = 0.0f, rise = 0.0f, size = 0.04f;
    };
    struct Fade {
        Vec4 color{0.0f};
        float from = 0.0f, to = 0.0f, left = 0.0f, total = 0.0f, now = 0.0f;
    };

    std::vector<Popup> m_popups;
    Fade m_fade;
};

} // namespace eruption
