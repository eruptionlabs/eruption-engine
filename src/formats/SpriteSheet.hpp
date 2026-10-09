#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace eruption {

// Folha de sprites (PNG em grade) -> ERUPTSPR versão 2, o formato que o
// motor carrega para personagens.
//
// ERUPTSPR v2 (little-endian):
//   char[8] "ERUPTSPR", u32 version = 2
//   u32 imageCount; por imagem: u32 w, u32 h, u8 rgba[w*h*4]
//   u32 actionCount; por ação:
//     u32 delayMs, u32 frameCount; por quadro:
//       u32 spriteCount; por sprite: u32 image, i32 offX, i32 offY,
//                                    f32 scaleX, f32 scaleY, f32 r, g, b, a, u32 flags (bit 0 = espelhado)
//
// Ações na ordem que o motor usa: 8 direções por estado (0-7 parado,
// 8-15 andando), vistas pela câmera: 0 de frente, 1 frente-direita,
// 2 direita da tela, 3 costas-direita, 4 costas, 5 costas-esquerda,
// 6 esquerda, 7 frente-esquerda.
struct SpriteSheetSpec {
    std::string image;          // PNG da folha
    int columns = 4, rows = 4;  // grade
    // Quantas direções a folha desenha: 1, 4 ou 8. Cada direção é uma linha.
    int directions = 4;
    // Para cada linha da folha (a partir da linha do estado), qual direção
    // do motor ela desenha (0..7). Tamanho = `directions`.
    std::vector<int> rowDirection{0, 6, 2, 4}; // frente, esquerda, direita, costas
    // Direção que falta desenhada espelhando a oposta (ex.: direita = esquerda espelhada).
    bool mirrorMissing = true;
    struct Anim {
        int firstRow = 0;    // linha da direção rowDirection[0]
        int firstColumn = 0;
        int frames = 1;
        int delayMs = 150;
        bool enabled = true;
    };
    Anim idle{0, 0, 1, 300, true};
    Anim walk{0, 0, 4, 150, true};
    float worldHeight = 25.0f; // altura do quadro (sem o vão do pé) no mundo
    int footOffset = 0;        // pixels transparentes abaixo do pé (cortados de cada quadro)
};

// Imagem já cortada e o plano de ações, para a prévia do editor e o gravador.
struct SpriteSheetResult {
    int cellW = 0, cellH = 0;
    std::vector<std::vector<uint8_t>> images; // RGBA cellW x cellH
    std::vector<std::pair<int, int>> imageCell; // (linha, coluna) de cada imagem na folha
    int sheetW = 0, sheetH = 0, strideH = 0;    // folha e altura da célula antes do corte do pé
    struct Frame { uint32_t image; bool mirror; };
    struct Action { uint32_t delayMs; std::vector<Frame> frames; };
    std::vector<Action> actions; // 16: parado 0-7, andando 8-15
    float scale = 1.0f;
};

bool buildSpriteSheet(const SpriteSheetSpec& spec, SpriteSheetResult& out, std::string& error);
bool writeEruptSprV2(const SpriteSheetResult& sheet, const std::string& path, std::string& error);

} // namespace eruption
