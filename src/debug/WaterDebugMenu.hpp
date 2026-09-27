#pragma once

#include <string>
#include <vector>

#include "renderer/WaterRenderer.hpp"
#include "math/Types.hpp"
#include <string>

namespace eruption {

class WaterDebugMenu {
public:
    struct LiquidEntry {
        std::string name;
        int kind = 0;
        float level = 0.0f;
        // Centro AUTORADO no .env (nao e' editavel aqui - so' pra saber
        // onde o gizmo comeca; o nudge de posicao soma offsetX/Z em cima
        // disso). Sincronizado por syncLiquidMenuFromMap, so' em map load.
        float centerX = 0.0f;
        float centerZ = 0.0f;
        // Nudge de posicao/rotacao POR CIMA dos valores autorados no .env
        // (nunca escreve no arquivo). So' tem efeito visivel pra liquido do
        // tipo Lava, que e' o unico com forma propria (disco lobado); a
        // agua principal e' um plano derivado do terreno, sem centro pra
        // mover. offsetX/Z e offsetY somam na posicao, rotationDeg gira o
        // perfil de raio-por-angulo (shoreRadius) em torno do centro.
        float offsetX = 0.0f;
        float offsetZ = 0.0f;
        float offsetY = 0.0f;
        float rotationDeg = 0.0f;
    };

    WaterRenderer::WaterSettings config;

    std::vector<LiquidEntry> liquids;
    int selectedLiquid = 0;

    void ensureDefaultLiquid();
    void applySelectedLiquid();

    // true depois que o usuario mexeu em posicao/rotacao de algum liquido
    // nesta janela - quem desenha o frame (Engine) precisa reconstruir a
    // malha do lago com os novos offsets e depois zerar esta flag.
    bool liquidShapeDirty = false;

    void loadConfig();
    void saveConfig();
    // view/proj: matrizes da camera ATUAL, so' pra desenhar o gizmo de
    // posicao/rotacao (ImGuizmo) da lava selecionada no viewport - nao
    // usadas pra mais nada nesta classe. screenW/H: tamanho do framebuffer
    // em pixels, pra ImGuizmo::SetRect cobrir a tela inteira.
    void drawUI(const Mat4& view, const Mat4& proj, float screenW, float screenH);

    bool settingsChanged = false;
    bool requestGenerateBlueNoise = false;
    bool requestLoadTexture = false;
    bool requestReloadShaders = false;
    uint32_t abTestMask = 0xFFFFFFFFu;

private:
    static constexpr const char* CONFIG_PATH = "data/water_config.json";
};

} // namespace eruption
