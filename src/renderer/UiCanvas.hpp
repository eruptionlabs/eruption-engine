#pragma once

// CANVAS 2D POR SCRIPT — interface montada pelo jogo, desenhada pelo motor.
//
// A HUD do motor (game/HudRenderer) desenha um conjunto FIXO de widgets com
// layout em CSS: serve pra barra de vida do herói, não pra um menu de combate,
// uma fila de ordem de turno ou a ficha de quatro personagens. Isso é
// interface do JOGO, e jogo nenhum cabe numa lista fechada de widgets.
//
// Então o motor não sabe o que é turno, menu ou barra de ATB: ele sabe
// desenhar RETÂNGULO, TEXTO e BARRA numa pilha de nós que o script cria,
// posiciona e pinta. Quem dá sentido a isso é o jogo.
//
// Nada de sistema de animação aqui: o script roda a cada frame (on.update) e
// move os próprios nós. Dois efeitos ficam no motor porque têm que ser
// exatos no frame e sobreviver ao script: os NÚMEROS que sobem (precisam da
// projeção de mundo pra tela) e o CLARÃO/FADE de tela cheia (transição).
//
// Coordenadas: fração da área de jogo (0..1 em x e y), para a interface
// valer igual em qualquer resolução e dentro do painel Cena do editor.
// Tamanho de texto em fração da ALTURA da área (0.03 ≈ 3% da tela).

#include "math/Types.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct ImDrawList;

namespace eruption {

class UiCanvas {
public:
    enum class Kind : uint8_t { Rect, Text, Bar };

    struct Node {
        uint32_t id = 0;
        Kind kind = Kind::Rect;
        bool alive = true;
        bool visible = true;
        float x = 0.0f, y = 0.0f, w = 0.1f, h = 0.05f;
        float z = 0.0f;                  // ordem de desenho (menor primeiro)
        Vec4 color{1.0f};                // retângulo: fundo; texto: cor; barra: trilho
        Vec4 color2{0.2f, 0.8f, 0.3f, 1.0f}; // retângulo: borda; barra: preenchimento
        float border = 0.0f;             // espessura da borda em pixels
        float radius = 0.0f;             // canto arredondado em pixels
        float value = 1.0f;              // barra: 0..1
        float textSize = 0.035f;         // fração da altura da área
        uint8_t align = 1;               // 0 esquerda, 1 centro, 2 direita
        bool shadow = true;              // texto com sombra de 1 px (legível em cima da cena)
        std::string text;
    };

    // Cria um nó e devolve o id (nunca 0). O id não se repete nesta sessão.
    uint32_t add(Kind kind);
    // Nó pelo id, ou nullptr se já foi removido.
    Node* get(uint32_t id);
    void remove(uint32_t id);
    void clear();
    size_t count() const { return m_nodes.size(); }

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

    std::vector<Node> m_nodes;   // por ordem de criação; o desenho ordena por z
    std::vector<Popup> m_popups;
    Fade m_fade;
    uint32_t m_nextId = 1;
    mutable std::vector<const Node*> m_sorted; // reaproveitado: sem alocar por frame
};

} // namespace eruption
