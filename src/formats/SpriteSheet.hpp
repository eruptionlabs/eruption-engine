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
// Versão 3 (com animações extras) acrescenta no fim:
//   u32 namedCount; por nome: u16 len, char name[len], u32 firstAction, u32 loop
//   (8 ações a partir de firstAction, uma por direção)
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
    // Animações extras com nome (atacar, pular, sentar...): scripts e regras tocam.
    struct NamedAnim {
        std::string name;
        Anim anim;
        bool loop = false; // falso: toca uma vez e volta ao parado
    };
    std::vector<NamedAnim> extra;
    float worldHeight = 25.0f; // altura do quadro (sem o vão do pé) no mundo
    // Linhas abaixo da linha do pé: a âncora no chão. Quadro com algo
    // desenhado ali (corpo deitado, poeira) guarda essas linhas e desce por
    // elas; os outros perdem o vão transparente, como sempre.
    int footOffset = 0;
};

// Imagem já cortada e o plano de ações, para a prévia do editor e o gravador.
struct SpriteSheetResult {
    int cellW = 0, cellH = 0;
    std::vector<std::vector<uint8_t>> images; // RGBA cellW x cellH
    std::vector<std::pair<int, int>> imageCell; // (linha, coluna) de cada imagem na folha
    std::vector<int> imageBelow; // linhas abaixo do pé que a imagem guarda (0 = cellH de altura)
    std::vector<int> imageTop;   // linhas vazias tiradas do topo (só em quadro com algo abaixo do pé)
    int sheetW = 0, sheetH = 0, strideH = 0;    // folha e altura da célula antes do corte do pé
    struct Frame { uint32_t image; bool mirror; };
    struct Action { uint32_t delayMs; std::vector<Frame> frames; };
    std::vector<Action> actions; // 16: parado 0-7, andando 8-15; depois 8 por animação extra
    struct Named { std::string name; uint32_t firstAction; bool loop; };
    std::vector<Named> named;
    float scale = 1.0f;
};

bool buildSpriteSheet(const SpriteSheetSpec& spec, SpriteSheetResult& out, std::string& error);
bool writeEruptSprV2(const SpriteSheetResult& sheet, const std::string& path, std::string& error);

// Receita de folha em JSON, ao lado do PNG (<nome>.sheet.json). E' o mesmo
// que o importador do editor monta na mao, escrito num arquivo: serve pra
// quem GERA sprite por programa (pixel art de placeholder, lote de NPC) e
// pra refazer o .spr sem abrir o editor.
//
//   { "image": "assets/sprites/heroi.png",
//     "columns": 14, "rows": 4, "directions": 4,
//     "rowDirection": [0, 6, 2, 4], "mirrorMissing": true,
//     "worldHeight": 28.0, "footOffset": 2,
//     "idle": { "firstRow": 0, "firstColumn": 0, "frames": 2, "delayMs": 450 },
//     "walk": { "firstRow": 0, "firstColumn": 2, "frames": 4, "delayMs": 140 },
//     "extra": [ { "name": "attack", "firstColumn": 6, "frames": 3,
//                  "delayMs": 90, "loop": false } ] }
//
// Campo ausente = padrao do SpriteSheetSpec. O caminho de "image" vale a
// partir da pasta do projeto (ou do proprio .sheet.json, se nao achar).
bool loadSpriteSheetRecipe(const std::string& jsonPath, SpriteSheetSpec& out, std::string& error);
// Le a receita, corta a folha e grava o .spr ao lado do PNG. Caminho do .spr
// gravado vai em *writtenPath.
bool importSpriteSheetRecipe(const std::string& jsonPath, std::string& error, std::string* writtenPath = nullptr);

} // namespace eruption
