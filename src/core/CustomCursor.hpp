#pragma once

#include <string>

struct GLFWwindow;

namespace eruption {

// Cursor do mouse escolhido pelo jogo (input.set_cursor). Padrao: nenhum, vale
// o cursor do sistema. Desenho por cursor de hardware (GLFWcursor, um por
// quadro na animacao, criados uma vez e trocados so' quando o quadro muda): o
// sistema o desenha por cima de tudo, sem custo de GPU. Se o sistema recusar o
// cursor (ou com ERUPTION_SOFTWARE_CURSOR=1 / opcao software), o cursor do
// sistema some sobre a janela e o motor desenha o cursor como sprite de UI no
// topo (no EndFrame do ImGui; nao aparece na tela de carga).
// Imagem ilegivel: cursor do sistema e aviso no log. Android e navegador: no-op.
struct CursorOptions {
    int frameW = 0, frameH = 0;  // 0 = imagem inteira e' um quadro
    int frames = 1;              // quadros da folha (esquerda->direita, linha a linha)
    float fps = 8.0f;            // so' vale com frames > 1
    float scale = 1.0f;          // multiplica a escala da janela (DPI)
    bool software = false;       // forca o desenho pelo motor (sem GLFWcursor)
};

namespace customcursor {

// Escolhe o cursor. Chamar de novo com os mesmos argumentos nao faz nada.
// path vazio = volta ao cursor do sistema.
void set(GLFWwindow* window, const std::string& path, int hotX, int hotY, const CursorOptions& opt);
// Avanca a animacao (chamado a cada quadro, barato quando nao ha animacao).
void tick(GLFWwindow* window);
// Destroi todos os cursores (antes de fechar a janela).
void shutdown(GLFWwindow* window);
// Quantos GLFWcursor ja foram criados (para testes).
int createdCount();

} // namespace customcursor
} // namespace eruption
