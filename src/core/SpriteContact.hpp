#pragma once
// CONTATO de um sprite: o que de fato toca o chao, lido dos proprios pixels
// (alfa) de cada quadro. Os pixels decidem QUANDO (uma mancha que aparece ou
// desce ate' o chao entre dois quadros), QUANTAS (dois pes, quatro patas, uma
// so' larga), de que TIPO (pisada ou arrasto) e a FORMA da marca: cada uma das
// 8 direcoes do sprite mostra o pe' de um lado (de perfil, o comprimento; de
// frente, a largura), e a forma no chao e' o que cabe em todas essas sombras
// ao mesmo tempo (casco visual, como uma tomografia de poucas vistas). Sem
// tabela de criatura nenhuma.
//
// Logica pura (sem Vulkan): a engine entrega as camadas de cada quadro ja'
// em unidades de mundo e recebe de volta o que pisa e onde (tests/
// test_sprite_contact.cpp).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eruption {

struct SprImage;

// As ultimas linhas opacas de uma textura, como bits (1 = alfa >= 128 ou
// indice != 0): o que sobra da imagem depois do envio a GPU, para o contato.
struct SpriteContactRows {
    static constexpr int kRows = 24; // linhas guardadas, de baixo para cima
    uint16_t width = 0, height = 0;
    int bottom = -1;                 // linha (de cima) mais baixa com opaco; -1 = vazia
    int rows = 0;                    // linhas guardadas (<= kRows)
    std::vector<uint8_t> bits;       // rows x ((width + 7) / 8); linha r = imagem[bottom - r]
    bool at(int r, int x) const { return (bits[size_t(r) * size_t((width + 7) / 8) + size_t(x >> 3)] >> (x & 7)) & 1; }
};
std::shared_ptr<const SpriteContactRows> extractContactRows(const SprImage& img);

namespace contact {

constexpr float kPixelToWorld = 0.15f; // um pixel de arte em mundo (a escala do sprite do personagem)
constexpr float kCell = 0.0375f;     // lado da celula da grade do quadro (mundo): 1/4 de pixel
constexpr float kProfH = 0.45f;      // altura acima do chao do ciclo que conta como sola (3 pixels de arte)

// Uma camada de um quadro, ja' no mundo: sx = centro no eixo da DIREITA da
// tela (relativo a ancora), up0 = altura da borda de baixo da imagem acima da
// ancora, pw/ph = tamanho de um pixel da imagem.
struct Layer {
    const SpriteContactRows* rows = nullptr;
    float sx = 0.0f, up0 = 0.0f, pw = 0.0f, ph = 0.0f;
    bool mirror = false;
};

struct Blob {
    int c0 = 0, c1 = 0;          // colunas [c0, c1] da grade do quadro
    float sxCenter = 0.0f;       // centro no eixo da direita (relativo a ancora)
    float width = 0.0f;          // largura desenhada (mundo)
};

struct Frame {
    float x0 = 0.0f;             // sx da coluna 0
    int n = 0;                   // colunas
    float lowest = 0.0f;         // altura do pixel mais baixo do quadro (acima da ancora)
    std::vector<float> low;      // por coluna: altura do pixel mais baixo dela (inf = vazia)
    std::vector<uint8_t> touch;  // por coluna: perto do pixel mais baixo DESTE quadro (o corpo que desliza)
    std::vector<uint8_t> touchG; // por coluna: perto do pixel mais baixo do CICLO (o pe' que pisa; o salto sai)
    std::vector<float> prof;     // por coluna: altura de pixel opaco ate' kProfH acima do chao do ciclo (o perfil da sola)
    std::vector<Blob> strikes;   // o que pisou neste quadro (mancha de touchG que aparece ou desce)
    Blob all;                    // o chao deste quadro numa faixa so' (o arrasto)
    bool any = false;
};

// Forma da marca, escolhida pelo ciclo do sprite.
enum class Shape { Sole, Paw, Blot, Trail };

struct Action {
    std::vector<Frame> frames;
    float ground = 0.0f;         // altura do pixel mais baixo do ciclo
    int maxRuns = 0;             // manchas separadas no chao do ciclo, no quadro que tem mais
    float hop = 0.0f;            // quanto o pixel mais baixo de um quadro sobe do chao do ciclo
    float landFrac = 0.0f;       // maior pisada / largura do desenho (1 = o corpo todo pousa)
    int strikeCount = 0;         // pisadas no ciclo inteiro: 2 = bipede, 4 = quadrupede, 1 = salta
    float bodyWidth = 0.0f;      // maior largura do desenho (mundo)
    Shape shape = Shape::Blot;   // o que o ciclo deixa no chao
    std::string printId;         // mascara da marca do sprite (casco das 8 direcoes), "contact:<hash>"
    float printWidth = 0.0f;     // largura da marca (mundo, de lado a lado do andar)
    float printLength = 0.0f;    // comprimento da marca (mundo, ao longo do andar)
    bool strideFallback = false; // pisa por pixel nao detectou o ritmo nesta direcao: passada, com a forma
    int slideFrames = 0;         // quadros em que algo novo toca o chao DO PROPRIO quadro (cobra: poucos)
    bool drag = false;           // o chao do quadro persiste e quase nada pisa nele: o corpo desliza
    bool any = false;
};

// Define se a acao e' arrasto (decidido de fora, pelas 8 direcoes) e o tipo de marca.
void setDrag(Action& a, bool drag);

// O tipo vale para o sprite todo (as 8 direcoes): vale o da maioria. Uma
// direcao de sola em que nao se acham 2 pisadas por ciclo cai na passada.
void setFamily(Action& a, Shape shape);

// Constroi o contato de uma acao (todos os quadros do ciclo).
Action build(const std::vector<std::vector<Layer>>& frames);

// Mascara RGBA quadrada da marca: o alfa e' quanto a marca cobre o chao
// (afundado e compactado), a cor e' quase branca com o grao (tingida pela
// `color`); o topo da imagem e' a frente do andar.
struct Mask {
    int size = 0;
    float side = 0.0f;      // lado do quadrado (mundo)
    float width = 0.0f;     // largura da marca (mundo)
    float length = 0.0f;    // comprimento da marca (mundo)
    std::vector<uint8_t> rgba;
    std::string id;
    bool trail = false;     // trecho de rastro (reserva propria de marcas vivas)
};

// Uma vista do sprite: o eixo da DIREITA da tela no referencial de quem anda
// (au = para a direita dele, av = para a frente) e o perfil da sola nesse
// eixo, centrado, uma amostra por kCell (0..1).
struct View {
    float au = 0.0f, av = 0.0f;
    std::vector<float> prof;
};

// O perfil medio da sola numa direcao: cada pe' (ou o corpo que arrasta) que
// toca o chao do ciclo, centrado no proprio meio e somado. false sem nenhum.
bool viewProfile(const Action& a, std::vector<float>& prof);

// A marca que cabe nas vistas: em cada ponto do chao, o perfil das vistas que
// passam por ele (consenso: as 2 mais estreitas de cada 8 nao cortam). Borda macia, fundo mais escuro no miolo e
// grao de terra.
Mask hullMask(const std::vector<View>& views, bool trail);

} // namespace contact
} // namespace eruption
