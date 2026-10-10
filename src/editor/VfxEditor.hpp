#pragma once

// EDITOR DE EFEITOS (Janela > Efeitos). Tres colunas:
//   - biblioteca: efeitos do jogo (assets/effects) e modelos do motor (data/vfx);
//   - previa isolada (VfxPreview) com camera orbital, tocar/pausar/recomecar;
//   - pilha do efeito: emissores e, para cada um, os modulos (nascimento,
//     forma, inicial, movimento, colisao, ao longo da vida, desenho), com
//     gradiente de cor e curva de tamanho editaveis.
// Toda mudanca vale na hora (previa e, se ligado, a instancia tocando na
// cena). Desfazer/refazer guarda o efeito inteiro em texto (mesmo formato do
// arquivo .vfx.json).

#include "editor/VfxGraphEditor.hpp"
#include "editor/VfxPreview.hpp"
#include "renderer/vfx/VfxEffect.hpp"

#include <string>
#include <vector>

namespace eruption {

class Engine;

class VfxEditor {
public:
    void init(Engine* engine);
    void shutdown();
    // Gancho offscreen do motor (fora de passe de render).
    void renderPreview(VkCommandBuffer cmd, float dt);
    // Desenha a janela; *open = false fecha.
    void draw(bool* open);
    // Abre um efeito pelo nome (biblioteca) ou caminho .vfx.json.
    bool openEffect(const std::string& nameOrPath);
    bool visible() const { return m_visible; }
    const std::string& currentName() const { return m_name; }
    int selectedEmitter() const { return m_sel; }
    void selectEmitter(int i) { m_sel = i; }
    void showGraph(bool on) { m_showGraph = on; }

private:
    struct LibraryItem {
        std::string name;
        std::string path;
        bool engineTemplate = false;
    };
    void refreshLibrary();
    void newEffect();
    bool save(const std::string& path);
    void applyLive();
    void trackUndo();
    void undo();
    void redo();
    void playInScene(bool on);

    void drawLibrary();
    void drawPreviewPane();
    void drawStack();
    bool drawEmitter(VfxEmitterDesc& e);
    bool subEmitterUi(const char* label, VfxSubEmitter& s, const std::string& self);
    bool gradientEditor(const char* id, VfxGradient& g);
    bool curveEditor(const char* id, VfxCurve& c);

    Engine* m_engine = nullptr;
    VfxPreview m_preview;
    VfxGraphEditor m_graphEditor;
    bool m_showGraph = false;
    bool m_visible = false;
    bool m_previewReady = false;

    std::vector<LibraryItem> m_library;
    std::string m_name;   // nome do efeito (chave no VfxSystem)
    std::string m_path;   // arquivo de onde veio (vazio = novo, ainda sem arquivo)
    VfxEffectDesc m_fx;
    int m_sel = 0;        // emissor selecionado
    bool m_dirty = false;
    int m_gradKey = 0;    // chave selecionada no gradiente

    // Desfazer: retratos em texto; o atual fica em m_snapshot.
    std::vector<std::string> m_undo, m_redo;
    std::string m_snapshot;
    std::string m_saved; // retrato do que esta' no arquivo (marca de nao salvo)

    uint32_t m_sceneInstance = 0; // tocando na cena (0 = nao)
    char m_saveAsName[64] = {};
    std::string m_status;
};

} // namespace eruption
