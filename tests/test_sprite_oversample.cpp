// Folha de sprite ampliada 4x e desenhada a 0,25: a regra que escolhe o
// filtro linear e o tamanho da normal embutida (bloco NRMS).
#include "formats/SpriteTypes.hpp"

#include <cstdio>

using namespace eruption;

static int s_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FALHOU: %s (linha %d)\n", #c, __LINE__); ++s_fail; } } while (0)

static AnimFile animWithScales(float a, float b) {
    AnimFile anim;
    ActAction action;
    ActFrame frame;
    ActSprite s1; s1.scaleX = s1.scaleY = a;
    ActSprite s2; s2.scaleX = s2.scaleY = b;
    frame.sprites = {s1, s2};
    action.frames.push_back(frame);
    anim.actions.push_back(action);
    return anim;
}

int main() {
    CHECK(spriteSheetOversampled(animWithScales(0.25f, 0.25f)));
    CHECK(spriteSheetOversampled(animWithScales(0.5f, 0.25f)));
    CHECK(!spriteSheetOversampled(animWithScales(1.0f, 1.0f)));
    CHECK(!spriteSheetOversampled(animWithScales(0.25f, 1.0f))); // camada de tamanho original: folha toda em nearest
    CHECK(!spriteSheetOversampled(AnimFile{}));                   // sem camada nenhuma

    uint32_t nw = 0, nh = 0;
    spriteNormalSize(96, 140, 4, nw, nh);
    CHECK(nw == 24 && nh == 35);
    spriteNormalSize(96, 140, 1, nw, nh); // NRML: mesmo tamanho
    CHECK(nw == 96 && nh == 140);
    spriteNormalSize(2, 3, 4, nw, nh);    // nunca zero
    CHECK(nw == 1 && nh == 1);
    spriteNormalSize(8, 8, 0, nw, nh);    // divisor invalido vale 1
    CHECK(nw == 8 && nh == 8);

    if (s_fail == 0) std::printf("test_sprite_oversample: ok\n");
    return s_fail == 0 ? 0 : 1;
}
