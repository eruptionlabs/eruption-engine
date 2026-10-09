#pragma once

#include "editor/EditorCommands.hpp"
#include "editor/RuleSheet.hpp"
#include "editor/UndoStack.hpp"
#include "core/Logger.hpp"
#include "renderer/DeferredLighting.hpp"
#include "math/Types.hpp"
#include "renderer/VulkanContext.hpp"

#include <deque>
#include <memory>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <string>
#include <vector>

namespace eruption {

class Engine;
class ScriptHost;
}
class TextEditor;
namespace eruption {

// Editor de cena: painéis encaixáveis em volta da imagem do jogo. Existe só
// com `--editor`; sem ele o motor roda o jogo como antes, sem custo nenhum.
//
// Ciclo por frame: update() antes do render (câmera de edição e atalhos) e
// drawUI() dentro do passe de ImGui do motor.
class Editor {
public:
    Editor();
    ~Editor();
    bool init(Engine& engine);
    void shutdown();

    void update(float dt);
    void drawUI();

    // Com o jogo parado (modo edição) a aplicação não roda a lógica de jogo.
    bool gameRunning() const { return m_play == PlayState::Playing || m_stepFrames > 0; }
    void consumeStep() { if (m_stepFrames > 0) --m_stepFrames; }
    bool showLegacyTools() const { return m_showLegacyTools; }
    // Play ou pausa (o jogo existe, mesmo congelado).
    bool inPlayMode() const { return m_play != PlayState::Editing; }
    void setScriptHost(ScriptHost* host) { m_scripts = host; }

private:
    enum class PlayState { Editing, Playing, Paused };
    enum class Tool { Select, Move, Rotate, Scale };
    enum class SelectionKind { None, Model, Light, Environment, Asset };
    struct Selection {
        SelectionKind kind = SelectionKind::None;
        int index = -1;
        bool operator==(const Selection& o) const { return kind == o.kind && index == o.index; }
    };
    struct ConsoleLine {
        LogLevel level;
        std::string text;
        uint32_t repeat = 1;
    };
    // Estado da cena guardado ao apertar Play e devolvido ao parar.
    struct PlaySnapshot {
        std::vector<Mat4> transforms;
        std::vector<bool> enabled;
        std::vector<PointLight> lights;
        Vec3 target{0.0f};
        float yaw = 0.0f, pitch = 0.0f, distance = 0.0f;
        float timeOfDay = 0.0f;
    };

    void registerCommands();
    void applyTheme();
    void buildDefaultLayout(unsigned int dockspaceId);

    void drawMenuBar();
    void drawToolbar();
    void drawStatusBar();
    void drawViewport();
    void drawHierarchy();
    void drawInspector();
    void drawProject();
    void drawConsole();
    void drawCommandPalette();
    void drawShortcutsWindow();
    void drawWelcome();
    void drawRules();
    void drawTelemetry();
    void recordFrameTime(float dt);
    void drawCode();
    void codeOpen(const std::filesystem::path& file);
    bool codeSave();

    // Texturas da interface (EditorAssets.cpp)
    struct GuiTexture {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t bindlessSlot = 0xFFFFFFFFu;
        int width = 0, height = 0;
        bool valid() const { return set != VK_NULL_HANDLE; }
    };
    GuiTexture makeTexture(const uint8_t* rgba, int w, int h, bool nearest);
    void releaseTexture(GuiTexture& t);
    uint32_t bindTexture(GuiTexture& t);
    void collectTextures(bool all);
    // Miniaturas do painel Projeto
    const GuiTexture* thumbnail(const std::string& path);
    void thumbnailWorker();
    void uploadThumbnails();
    void stopThumbnails();
    // Prévia de asset no Inspetor
    void selectAsset(const std::filesystem::path& path);
    void closeAssetPreview();
    void drawAssetPreview();
    void drawImagePreview();
    void drawModelPreview();
    void drawMaterialPreview();
    void rulesLoad(const std::filesystem::path& file);
    void rulesCommit(const std::string& label);
    bool rulesChip(RuleCall& call, int id, ImU32 color);
    bool rulesParamEditor(RuleCall& call);
    void rulesAddPopup();

    // Viewport (EditorViewport.cpp)
    void updateCamera(float dt);
    void frameSelection();
    void pickAt(float u, float v);
    void drawGizmo(float x, float y, float w, float h);
    void drawViewCube(float x, float y, float w, float h);
    void drawSelectionBounds(float x, float y, float w, float h);
    void cameraLookDir(const Vec3& forward);

    // Seleção e edição (EditorPanels.cpp)
    void select(const Selection& s);
    std::string selectionName(const Selection& s) const;
    bool selectionHasTransform() const;
    Mat4 selectionTransform() const;
    void setSelectionTransform(const Mat4& m);
    void pushTransformEdit(const Selection& s, const Mat4& before, const Mat4& after);
    void deleteSelection();

    void play();
    void stop();
    void togglePause();
    void step();

    void openInExternalEditor(const std::filesystem::path& file, int line = 0);
    // "caminho.luau:12: ..." -> abre o arquivo na linha. Falso se não casar.
    bool openSourceReference(const std::string& text);
    void log(LogLevel level, const std::string& text);
    static void logSink(LogLevel level, const char* message, void* user);

    Engine* m_engine = nullptr;
    ScriptHost* m_scripts = nullptr;
    EditorCommands m_commands;
    UndoStack m_undo;

    PlayState m_play = PlayState::Editing;
    int m_stepFrames = 0;
    PlaySnapshot m_snapshot;

    Tool m_tool = Tool::Move;
    bool m_localSpace = false;
    bool m_snap = false;
    float m_snapMove = 1.0f, m_snapAngle = 15.0f, m_snapScale = 0.1f;
    Selection m_selection;

    // Câmera de edição
    float m_flySpeed = 30.0f;
    bool m_flying = false;
    bool m_cameraDrag = false;
    float m_savedMaxDistance = 0.0f;
    bool m_savedFreeCamera = false;

    // Viewport
    bool m_viewportHovered = false;
    bool m_viewportFocused = false;
    float m_viewportRect[4] = {0, 0, 0, 0};
    uint32_t m_pendingW = 0, m_pendingH = 0;
    float m_resizeTimer = 0.0f;
    bool m_gizmoWasUsing = false;
    Mat4 m_gizmoBefore{1.0f};
    Selection m_gizmoSelection;
    bool m_inspectorEditing = false;
    Mat4 m_inspectorBefore{1.0f};

    // Painéis
    bool m_resetLayout = false;
    bool m_focusScene = false;
    std::vector<const char*> m_pendingFocus; // abas a trazer para frente no próximo desenho
    bool m_showTelemetry = true;
    static constexpr int kFrameHistory = 240;
    float m_frameTimes[kFrameHistory] = {};
    int m_frameTimeHead = 0; // layout recém-montado: aba Cena na frente
    bool m_showHierarchy = true, m_showInspector = true, m_showProject = true, m_showConsole = true;
    bool m_showShortcuts = false;
    bool m_showLegacyTools = false;
    bool m_showWelcome = true;
    bool m_paletteOpen = false;
    bool m_paletteFocus = false;
    char m_paletteQuery[128] = {};
    int m_paletteIndex = 0;
    char m_hierarchyFilter[128] = {};
    char m_consoleFilter[128] = {};
    bool m_consoleShow[3] = {true, true, true}; // info, aviso, erro
    bool m_consoleAutoScroll = true;
    std::filesystem::path m_projectRoot;
    std::filesystem::path m_projectDir;
    std::string m_projectSelected;
    std::string m_pendingMap;
    std::string m_lastMap;
    std::string m_hardware; // "GPU | CPU" na barra de status
    bool m_framedSpawn = false;

    // Folha de eventos
    struct RulesState {
        std::filesystem::path file;
        RuleSheet sheet;
        std::string error;
        std::string savedText;
        std::filesystem::file_time_type mtime{};
        bool showCode = true;
        char search[64] = {};
        int addRule = -1;  // regra que recebe o item do popup (-1: regra nova)
        int addList = 0;   // 0 condição, 1 ação, 2 senão, 3 evento de regra nova
        bool openAdd = false;
        char newName[64] = "rules";
    };
    RulesState m_rules;
    struct CodeState {
        std::unique_ptr<::TextEditor> editor;
        std::filesystem::path file;
        std::string saved;
        std::string checked;
        std::filesystem::file_time_type mtime{};
        float checkTimer = 0.0f;
        int errors = 0;
        bool focus = false;
    };
    CodeState m_code;

    // Texturas liberadas esperam alguns frames: a GPU ainda pode estar lendo.
    struct DeadTexture { GuiTexture tex; uint64_t frame; };
    std::vector<DeadTexture> m_deadTextures;
    uint64_t m_frame = 0;

    struct Thumb {
        int state = 0; // 0 pedido, 1 pronto, 2 falhou
        GuiTexture tex;
        uint64_t lastUsed = 0;
    };
    struct ThumbResult { std::string path; std::vector<uint8_t> rgba; int w = 0, h = 0; };
    std::unordered_map<std::string, Thumb> m_thumbs;
    std::mutex m_thumbMutex;
    std::condition_variable m_thumbCv;
    std::deque<std::string> m_thumbQueue;
    std::deque<ThumbResult> m_thumbDone;
    std::thread m_thumbThread;
    bool m_thumbQuit = false;
    bool m_projectGrid = true;

    struct AssetPreviewState {
        std::filesystem::path path;
        enum class Kind { None, Image, Model, Text } kind = Kind::None;
        // Imagem
        std::vector<uint8_t> rgba;     // já reduzida para no máximo 1024
        int width = 0, height = 0;     // tamanho original
        int fileChannels = 0;
        int view = 0;                  // 0 cor, 1 R, 2 G, 3 B, 4 A
        bool pixelated = false;
        GuiTexture tex;                // vista atual
        // Material PBR achado ao lado do arquivo
        std::filesystem::path albedoPath, mrahwPath, normalPath;
        GuiTexture albedo, mrahw, normal, pbrChannels[4];
        bool materialReady = false;
        bool cube = false;
        // Modelo
        std::string modelInfo;
        struct Image { std::string name, mime; size_t offset = 0, size = 0; std::string uri; };
        std::vector<Image> images;
        int imageShown = -1;
        GuiTexture embedded;
        int instances = 0;
        // Texto
        std::string text;
    };
    AssetPreviewState m_asset;
    bool m_showCode = true;
    UiLanguage m_lang = UiLanguage::English;
    bool m_showRules = true;

    std::mutex m_consoleMutex;
    std::deque<ConsoleLine> m_console;
    uint32_t m_consoleCounts[3] = {0, 0, 0};
};

} // namespace eruption
