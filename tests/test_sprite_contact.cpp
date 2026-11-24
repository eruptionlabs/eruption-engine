// Contato do sprite: dois pes de um bipede (passo = pe' que aparece no chao) e
// uma faixa sem pernas (arrasto). Logica pura, sem GPU.
#include "core/SpriteContact.hpp"
#include "formats/SpriteTypes.hpp"

#include <algorithm>
#include <cstdio>

using namespace eruption;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FALHOU %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

// Imagem 16x16 com retangulos opacos [x0,x1) x [y0,y1) (y de cima).
static SprImage makeImage(std::initializer_list<int> rects) {
    SprImage img; img.width = 16; img.height = 16; img.pixels.assign(16 * 16 * 4, 0);
    const int* r = rects.begin();
    for (size_t i = 0; i + 3 < rects.size(); i += 4)
        for (int y = r[i + 2]; y < r[i + 3]; ++y)
            for (int x = r[i]; x < r[i + 1]; ++x) img.pixels[(size_t(y) * 16 + size_t(x)) * 4 + 3] = 255;
    return img;
}

static contact::Layer layerOf(const SpriteContactRows& rows) {
    contact::Layer l; l.rows = &rows; l.sx = 0.0f; l.up0 = 0.0f;
    l.pw = l.ph = contact::kPixelToWorld; return l;
}

int main() {
    // Bipede: ciclo A (pe' esquerdo no chao), M (os dois no ar), B (pe' direito
    // no chao), M. Cada pe' que pousa depois de um quadro no ar e' um passo.
    auto a = extractContactRows(makeImage({ 2, 6, 4, 16, 10, 14, 4, 12 }));
    auto m0 = extractContactRows(makeImage({ 2, 6, 4, 12, 10, 14, 4, 12 }));
    auto b = extractContactRows(makeImage({ 2, 6, 4, 12, 10, 14, 4, 16 }));
    CHECK(a && a->bottom == 15);
    std::vector<std::vector<contact::Layer>> frames = { { layerOf(*a) }, { layerOf(*m0) }, { layerOf(*b) }, { layerOf(*m0) } };
    contact::Action act = contact::build(frames);
    CHECK(act.any);
    CHECK(!act.drag);
    CHECK(act.frames.size() == 4);
    int steps = 0;
    for (const auto& f : act.frames)
        for (const auto& s : f.strikes) { ++steps; CHECK(s.width > 0.4f && s.width < 1.2f); }
    CHECK(steps == 2);

    // Corpo sem pernas: faixa baixa e continua que desliza; o chao persiste, nada pisa.
    auto s1 = extractContactRows(makeImage({ 0, 12, 13, 16 }));
    auto s2 = extractContactRows(makeImage({ 2, 14, 13, 16 }));
    std::vector<std::vector<contact::Layer>> strip = { { layerOf(*s1) }, { layerOf(*s2) } };
    contact::Action sa = contact::build(strip);
    CHECK(sa.any);
    CHECK(sa.frames[0].all.width > 1.0f);

    // O ciclo decide o tipo: dois pes = sola; a faixa sem pernas = arrasto.
    CHECK(act.shape == contact::Shape::Sole);
    CHECK(sa.drag && sa.shape == contact::Shape::Trail);

    // Perfil de uma vista: centrado, pico 1.
    std::vector<float> prof;
    CHECK(contact::viewProfile(act, prof));
    CHECK(!prof.empty() && *std::max_element(prof.begin(), prof.end()) == 1.0f);

    // Casco das vistas: pe' longo de perfil (eixo da frente) e estreito de
    // frente (eixo do lado) da' marca mais comprida que larga.
    contact::View side, front;
    side.au = 0.0f; side.av = 1.0f; side.prof.assign(31, 0.0f);
    front.au = 1.0f; front.av = 0.0f; front.prof.assign(31, 0.0f);
    for (int k = 4; k < 27; ++k) side.prof[size_t(k)] = 1.0f;   // ~0,86 de comprimento
    for (int k = 11; k < 20; ++k) front.prof[size_t(k)] = 1.0f; // ~0,34 de largura
    contact::Mask m = contact::hullMask({ side, front }, false);
    CHECK(m.size >= 32 && m.size % 2 == 0);
    CHECK(m.rgba.size() == size_t(m.size) * size_t(m.size) * 4);
    CHECK(m.rgba[3] == 0);
    CHECK(m.length > 1.5f * m.width && m.width > 0.15f);
    CHECK(m.id.rfind("contact:", 0) == 0);

    // Uma direcao de sola sem 2 pisadas cai na passada.
    contact::Action few = act;
    few.strikeCount = 1;
    contact::setFamily(few, contact::Shape::Sole);
    CHECK(few.strideFallback && few.shape == contact::Shape::Sole);
    contact::setFamily(act, contact::Shape::Sole);
    CHECK(!act.strideFallback);

    if (!g_fail) std::printf("test_sprite_contact: OK\n");
    return g_fail;
}
