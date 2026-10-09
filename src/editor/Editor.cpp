#include "editor/Editor.hpp"
#include "editor/EditorTheme.hpp"

#include "core/Engine.hpp"
#include "script/ScriptHost.hpp"
#include "renderer/WeatherTypes.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace eruption {

namespace {

constexpr const char* kWinScene = "Scene###Scene";
constexpr const char* kWinHierarchy = "Hierarchy###Hierarchy";
constexpr const char* kWinInspector = "Inspector###Inspector";
constexpr const char* kWinProject = "Project###Project";
constexpr const char* kWinConsole = "Console###Console";
constexpr const char* kWinRules = "Rules###Rules";
constexpr const char* kWinCode = "Code###Code";


bool containsAllWords(const std::string& text, const char* query) {
    std::string hay = text;
    std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return std::tolower(c); });
    const char* p = query;
    while (*p) {
        while (*p == ' ') ++p;
        const char* start = p;
        while (*p && *p != ' ') ++p;
        if (p == start) break;
        std::string word(start, p);
        std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return std::tolower(c); });
        if (hay.find(word) == std::string::npos) return false;
    }
    return true;
}

} // namespace

bool Editor::init(Engine& engine) {
    m_engine = &engine;
    m_projectRoot = std::filesystem::current_path();
    m_projectDir = m_projectRoot;

    m_showWelcome = !std::filesystem::exists(ImGui::GetIO().IniFilename ? ImGui::GetIO().IniFilename : "");

    Logger::setSink(&Editor::logSink, this);
    registerCommands();

    // Preferências do editor no mesmo arquivo do layout.
    ImGuiSettingsHandler handler;
    handler.TypeName = "EruptionEditor";
    handler.TypeHash = ImHashStr("EruptionEditor");
    handler.UserData = this;
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char*) -> void* { return reinterpret_cast<void*>(1); };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler* h, void*, const char* line) {
        Editor* e = static_cast<Editor*>(h->UserData);
        int v = 0;
        if (std::sscanf(line, "Language=%d", &v) == 1) e->m_lang = v == 1 ? UiLanguage::Portuguese : UiLanguage::English;
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
        const Editor* e = static_cast<const Editor*>(h->UserData);
        buf->appendf("[%s][Settings]\nLanguage=%d\n\n", h->TypeName, e->m_lang == UiLanguage::Portuguese ? 1 : 0);
    };
    ImGui::AddSettingsHandler(&handler);
    if (ImGui::GetIO().IniFilename && std::filesystem::exists(ImGui::GetIO().IniFilename))
        ImGui::LoadIniSettingsFromDisk(ImGui::GetIO().IniFilename);
    applyTheme();

    Camera& cam = engine.camera();
    m_savedMaxDistance = cam.maxOrbitDistance();
    m_savedFreeCamera = cam.debugFreeCamera();
    cam.setMaxOrbitDistance(20000.0f);
    cam.setDebugFreeCamera(true);

    log(LogLevel::Info, "Editor ready. Press Ctrl+Shift+P to search every command.");
    return true;
}

void Editor::shutdown() {
    Logger::setSink(nullptr, nullptr);
    if (m_engine) m_engine->setEditor(nullptr);
    m_engine = nullptr;
}

void Editor::logSink(LogLevel level, const char* message, void* user) {
    static_cast<Editor*>(user)->log(level, message);
}

void Editor::log(LogLevel level, const std::string& text) {
    const int bucket = level >= LogLevel::Error ? 2 : (level == LogLevel::Warning ? 1 : 0);
    std::lock_guard<std::mutex> lock(m_consoleMutex);
    // Mensagens repetidas em sequência viram uma linha com contador.
    if (!m_console.empty() && m_console.back().level == level && m_console.back().text == text) {
        ++m_console.back().repeat;
    } else {
        m_console.push_back({level, text, 1});
        if (m_console.size() > 2000) m_console.pop_front();
    }
    ++m_consoleCounts[bucket];
}

void Editor::registerCommands() {
    using Scope = EditorCommand::Scope;
    auto add = [this](const char* id, const char* category, const char* label, Shortcut sc,
                      std::function<void()> run, const char* help = "") -> EditorCommand& {
        EditorCommand c;
        c.id = id;
        c.category = category;
        c.label = label;
        c.shortcut = sc;
        c.run = std::move(run);
        c.help = help;
        return m_commands.add(std::move(c));
    };
    auto isEditing = [this] { return m_play == PlayState::Editing; };

    // Arquivo
    add("file.open_map", "File", "Open Map...", {ImGuiKey_O, true}, [this] {
        m_paletteOpen = true;
        m_paletteFocus = true;
        std::snprintf(m_paletteQuery, sizeof(m_paletteQuery), "map ");
    }, "Search and load one of the available maps.");
    add("file.open_vscode", "File", "Open Project in VS Code", {}, [this] {
        openInExternalEditor(m_projectRoot);
    }, "Opens the project folder in Visual Studio Code (or the system default).");
    add("file.quit", "File", "Quit", {ImGuiKey_Q, true}, [this] {
        glfwSetWindowShouldClose(m_engine->m_window.handle(), GLFW_TRUE);
    });

    // Editar
    add("edit.undo", "Edit", "Undo", {ImGuiKey_Z, true}, [this] {
        const std::string label = m_undo.undoLabel();
        if (m_undo.undo()) log(LogLevel::Info, "Undo: " + label);
    }).enabled = [this] { return m_undo.canUndo(); };
    {
        auto& c = add("edit.redo", "Edit", "Redo", {ImGuiKey_Y, true}, [this] {
            const std::string label = m_undo.redoLabel();
            if (m_undo.redo()) log(LogLevel::Info, "Redo: " + label);
        });
        c.altShortcut = {ImGuiKey_Z, true, true};
        c.enabled = [this] { return m_undo.canRedo(); };
    }
    add("edit.delete", "Edit", "Delete", {ImGuiKey_Delete}, [this] { deleteSelection(); },
        "Hides the selected object (undo brings it back).")
        .enabled = [this] { return m_selection.kind == SelectionKind::Model || m_selection.kind == SelectionKind::Light; };
    {
        auto& c = add("edit.deselect", "Edit", "Deselect", {ImGuiKey_Escape}, [this] { select({}); });
        c.scope = Scope::Viewport;
        c.enabled = [this] { return m_selection.kind != SelectionKind::None; };
    }
    {
        auto& c = add("edit.frame", "Edit", "Frame Selection", {ImGuiKey_F}, [this] { frameSelection(); },
                      "Moves the camera to show the selected object.");
        c.scope = Scope::Viewport;
        c.enabled = [this] { return selectionHasTransform(); };
    }

    // Ferramentas
    auto tool = [&](const char* id, const char* label, ImGuiKey key, Tool t, const char* help) {
        auto& c = add(id, "Tools", label, {key}, [this, t] { m_tool = t; }, help);
        c.scope = Scope::Viewport;
        c.enabled = isEditing;
        c.checked = [this, t] { return m_tool == t; };
    };
    tool("tool.select", "Select", ImGuiKey_Q, Tool::Select, "Click objects to select them, no handles.");
    tool("tool.move", "Move", ImGuiKey_W, Tool::Move, "Drag the arrows to move. Hold Ctrl to snap.");
    tool("tool.rotate", "Rotate", ImGuiKey_E, Tool::Rotate, "Drag the rings to rotate. Hold Ctrl to snap.");
    tool("tool.scale", "Scale", ImGuiKey_R, Tool::Scale, "Drag the handles to scale. Hold Ctrl to snap.");
    {
        auto& c = add("tool.space", "Tools", "Toggle Local/World Space", {ImGuiKey_X},
                      [this] { m_localSpace = !m_localSpace; },
                      "Handles follow the object's own axes (local) or the world axes.");
        c.scope = Scope::Viewport;
        c.enabled = isEditing;
        c.checked = [this] { return m_localSpace; };
    }
    add("tool.snap", "Tools", "Toggle Snapping", {}, [this] { m_snap = !m_snap; },
        "Always snap to the grid steps. Holding Ctrl while dragging does the same.")
        .checked = [this] { return m_snap; };

    // Jogo
    {
        auto& c = add("game.play", "Game", "Play / Stop", {ImGuiKey_F5}, [this] {
            if (m_play == PlayState::Editing) play(); else stop();
        }, "Runs the game inside the Scene view. Stopping restores the scene.");
        c.altShortcut = {ImGuiKey_P, true};
        c.checked = [this] { return m_play != PlayState::Editing; };
    }
    add("game.stop", "Game", "Stop", {ImGuiKey_F5, false, true}, [this] { stop(); })
        .enabled = [this] { return m_play != PlayState::Editing; };
    add("game.pause", "Game", "Pause", {ImGuiKey_F6}, [this] { togglePause(); })
        .enabled = [this] { return m_play != PlayState::Editing; };
    add("game.step", "Game", "Step One Frame", {ImGuiKey_F10}, [this] { step(); })
        .enabled = [this] { return m_play == PlayState::Paused; };

    add("game.new_script", "Game", "New Script", {}, [this] {
        if (!m_scripts) return;
        const auto p = m_scripts->createScript("new_script");
        log(LogLevel::Info, "Created " + p.generic_string());
        m_projectDir = p.parent_path();
        m_projectSelected = p.string();
        codeOpen(p);
    }, "Creates a Luau script in the scripts folder and opens it.")
        .enabled = [this] { return m_scripts != nullptr; };
    add("game.open_scripts", "Game", "Open Scripts Folder in VS Code", {}, [this] {
        if (m_scripts) openInExternalEditor(m_scripts->dir());
    }, "Autocomplete for the engine API comes from eruption.d.luau (luau-lsp extension).")
        .enabled = [this] { return m_scripts != nullptr; };

    // Janela
    auto panel = [&](const char* id, const char* label, bool* flag) {
        add(id, "Window", label, {}, [flag] { *flag = !*flag; }).checked = [flag] { return *flag; };
    };
    panel("window.hierarchy", "Hierarchy", &m_showHierarchy);
    panel("window.inspector", "Inspector", &m_showInspector);
    panel("window.project", "Project", &m_showProject);
    panel("window.console", "Console", &m_showConsole);
    panel("window.rules", "Rules (event sheet)", &m_showRules);
    panel("window.code", "Code", &m_showCode);
    add("window.lang_en", "Window", "Language: English", {}, [this] {
        m_lang = UiLanguage::English;
        ImGui::MarkIniSettingsDirty();
    }).checked = [this] { return m_lang == UiLanguage::English; };
    add("window.lang_pt", "Window", "Idioma: Português", {}, [this] {
        m_lang = UiLanguage::Portuguese;
        ImGui::MarkIniSettingsDirty();
    }).checked = [this] { return m_lang == UiLanguage::Portuguese; };
    add("window.reset_layout", "Window", "Reset Layout", {}, [this] { m_resetLayout = true; },
        "Puts every panel back in its default place.");
    add("window.legacy", "Window", "Engine Debug Tools", {ImGuiKey_F12}, [this] {
        m_showLegacyTools = !m_showLegacyTools;
    }, "Shows the engine's own debug panels (telemetry, effects, weather).")
        .checked = [this] { return m_showLegacyTools; };

    // Ajuda
    {
        auto& c = add("help.palette", "Help", "Command Palette", {ImGuiKey_P, true, true}, [this] {
            m_paletteOpen = true;
            m_paletteFocus = true;
            m_paletteQuery[0] = '\0';
        }, "Search any command by name.");
        c.altShortcut = {ImGuiKey_K, true};
    }
    add("help.shortcuts", "Help", "Keyboard Shortcuts", {ImGuiKey_Slash, true}, [this] {
        m_showShortcuts = !m_showShortcuts;
    }).checked = [this] { return m_showShortcuts; };
    add("help.welcome", "Help", "Getting Started", {}, [this] { m_showWelcome = true; });
}

void Editor::applyTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 4.0f;
    st.FrameRounding = 3.0f;
    st.TabRounding = 3.0f;
    st.PopupRounding = 4.0f;
    st.GrabRounding = 3.0f;
    st.ScrollbarRounding = 6.0f;
    st.WindowBorderSize = 1.0f;
    st.FrameBorderSize = 0.0f;
    st.WindowPadding = ImVec2(8, 8);
    st.FramePadding = ImVec2(6, 4);
    st.ItemSpacing = ImVec2(8, 5);
    st.IndentSpacing = 14.0f;
    st.WindowMenuButtonPosition = ImGuiDir_None;

    ImVec4* c = st.Colors;
    const ImVec4 bg0(0.105f, 0.108f, 0.115f, 1.0f);
    const ImVec4 bg1(0.135f, 0.138f, 0.147f, 1.0f);
    const ImVec4 bg2(0.180f, 0.184f, 0.196f, 1.0f);
    const ImVec4 bg3(0.235f, 0.240f, 0.255f, 1.0f);
    const ImVec4 accentDim(kAccent.x * 0.75f, kAccent.y * 0.75f, kAccent.z * 0.75f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.91f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.51f, 0.53f, 1.0f);
    c[ImGuiCol_WindowBg] = bg1;
    c[ImGuiCol_ChildBg] = bg1;
    c[ImGuiCol_PopupBg] = bg0;
    c[ImGuiCol_Border] = ImVec4(0.06f, 0.06f, 0.07f, 1.0f);
    c[ImGuiCol_FrameBg] = bg0;
    c[ImGuiCol_FrameBgHovered] = bg2;
    c[ImGuiCol_FrameBgActive] = bg3;
    c[ImGuiCol_TitleBg] = bg0;
    c[ImGuiCol_TitleBgActive] = bg0;
    c[ImGuiCol_MenuBarBg] = bg0;
    c[ImGuiCol_ScrollbarBg] = bg1;
    c[ImGuiCol_Header] = bg3;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.31f, 0.33f, 1.0f);
    c[ImGuiCol_HeaderActive] = accentDim;
    c[ImGuiCol_Button] = bg2;
    c[ImGuiCol_ButtonHovered] = bg3;
    c[ImGuiCol_ButtonActive] = accentDim;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = accentDim;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Separator] = ImVec4(0.06f, 0.06f, 0.07f, 1.0f);
    c[ImGuiCol_SeparatorHovered] = accentDim;
    c[ImGuiCol_SeparatorActive] = kAccent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab] = bg0;
    c[ImGuiCol_TabHovered] = bg3;
    c[ImGuiCol_TabActive] = bg2;
    c[ImGuiCol_TabUnfocused] = bg0;
    c[ImGuiCol_TabUnfocusedActive] = bg1;
    c[ImGuiCol_DockingPreview] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = bg0;
    c[ImGuiCol_TextSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
    c[ImGuiCol_NavHighlight] = kAccent;
}

void Editor::buildDefaultLayout(unsigned int dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspaceId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.27f, nullptr, &center);
    ImGui::DockBuilderDockWindow(kWinRules, center);
    ImGui::DockBuilderDockWindow(kWinCode, center);
    ImGui::DockBuilderDockWindow(kWinScene, center);
    ImGui::DockBuilderDockWindow(kWinHierarchy, left);
    ImGui::DockBuilderDockWindow(kWinInspector, right);
    ImGui::DockBuilderDockWindow(kWinProject, bottom);
    ImGui::DockBuilderDockWindow(kWinConsole, bottom);
    ImGui::DockBuilderFinish(dockspaceId);
    m_showHierarchy = m_showInspector = m_showProject = m_showConsole = m_showRules = m_showCode = true;
}

void Editor::update(float dt) {
    if (!m_engine) return;

    // Troca de mapa pedida pela UI: fora do frame de render.
    if (!m_pendingMap.empty()) {
        const std::string name = std::move(m_pendingMap);
        m_pendingMap.clear();
        if (m_play != PlayState::Editing) stop();
        select({});
        m_undo.clear();
        log(LogLevel::Info, "Loading map " + name + "...");
        m_engine->loadMap(name);
    }

    // O painel Cena muda de tamanho enquanto se arrasta a divisória: só
    // recria os alvos de render quando o tamanho para de mudar.
    if (m_pendingW && m_pendingH) {
        m_resizeTimer += dt;
        if (m_resizeTimer > 0.12f || !m_engine->m_viewportW) {
            m_engine->setViewportSize(m_pendingW, m_pendingH);
            m_pendingW = m_pendingH = 0;
        }
    }

    // Mapa novo: depois que o jogador nasce (o motor reposiciona a câmera
    // nesse momento), começa numa vista de cima olhando o ponto de entrada.
    if (m_engine->currentMapName() != m_lastMap) {
        m_lastMap = m_engine->currentMapName();
        m_framedSpawn = false;
    }
    if (!m_framedSpawn && m_engine->m_playerSpawnedOnActiveMap && m_play == PlayState::Editing) {
        m_framedSpawn = true;
        Camera& cam = m_engine->camera();
        cam.setOrbit(cam.orbitYaw(), 0.85f, 500.0f);
    }

    if (m_play != PlayState::Playing) updateCamera(dt);
}

void Editor::drawUI() {
    if (!m_engine) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();

    drawMenuBar();

    const ImGuiWindowFlags barFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_MenuBar;
    const float toolbarH = ImGui::GetFrameHeight() + 2.0f;
    if (ImGui::BeginViewportSideBar("##Toolbar", vp, ImGuiDir_Up, toolbarH, barFlags)) {
        if (ImGui::BeginMenuBar()) {
            drawToolbar();
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
    if (ImGui::BeginViewportSideBar("##StatusBar", vp, ImGuiDir_Down, ImGui::GetFrameHeight(), barFlags)) {
        if (ImGui::BeginMenuBar()) {
            drawStatusBar();
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();

    // Janela hospedeira das abas, do tamanho da área livre.
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##EditorHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);
    const ImGuiID dockId = ImGui::GetID("EditorDockspace");
    // Sem arquivo de layout (primeira vez) ou depois de "Reset Layout".
    if (m_resetLayout || ImGui::DockBuilderGetNode(dockId) == nullptr) {
        buildDefaultLayout(dockId);
        m_resetLayout = false;
    }
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();

    if (m_showRules) drawRules();
    if (m_showCode) drawCode();
    drawViewport();
    if (m_showHierarchy) drawHierarchy();
    if (m_showInspector) drawInspector();
    if (m_showProject) drawProject();
    if (m_showConsole) drawConsole();
    if (m_showShortcuts) drawShortcutsWindow();
    if (m_paletteOpen) drawCommandPalette();

    m_commands.dispatchShortcuts((m_viewportHovered || m_viewportFocused) && m_play != PlayState::Playing);
}

void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        m_commands.menuItem("file.open_map");
        if (ImGui::BeginMenu("Recent Maps", !m_engine->availableMaps().empty())) {
            for (const auto& name : m_engine->availableMaps())
                if (ImGui::MenuItem(name.c_str(), nullptr, name == m_engine->currentMapName()))
                    m_pendingMap = name;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        m_commands.menuItem("file.open_vscode");
        ImGui::Separator();
        m_commands.menuItem("file.quit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const EditorCommand* u = m_commands.find("edit.undo");
        const EditorCommand* r = m_commands.find("edit.redo");
        const std::string ul = m_undo.canUndo() ? "Undo " + m_undo.undoLabel() : "Undo";
        const std::string rl = m_undo.canRedo() ? "Redo " + m_undo.redoLabel() : "Redo";
        if (ImGui::MenuItem(ul.c_str(), EditorCommands::shortcutText(u->shortcut).c_str(), false, m_undo.canUndo()))
            m_commands.run("edit.undo");
        if (ImGui::MenuItem(rl.c_str(), EditorCommands::shortcutText(r->shortcut).c_str(), false, m_undo.canRedo()))
            m_commands.run("edit.redo");
        ImGui::Separator();
        m_commands.menuItem("edit.delete");
        m_commands.menuItem("edit.deselect");
        m_commands.menuItem("edit.frame");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        m_commands.menuItem("tool.select");
        m_commands.menuItem("tool.move");
        m_commands.menuItem("tool.rotate");
        m_commands.menuItem("tool.scale");
        ImGui::Separator();
        m_commands.menuItem("tool.space");
        m_commands.menuItem("tool.snap");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Game")) {
        m_commands.menuItem("game.play");
        m_commands.menuItem("game.stop");
        m_commands.menuItem("game.pause");
        m_commands.menuItem("game.step");
        ImGui::Separator();
        m_commands.menuItem("game.new_script");
        m_commands.menuItem("game.open_scripts");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
        m_commands.menuItem("window.hierarchy");
        m_commands.menuItem("window.inspector");
        m_commands.menuItem("window.project");
        m_commands.menuItem("window.console");
        m_commands.menuItem("window.rules");
        m_commands.menuItem("window.code");
        ImGui::Separator();
        if (ImGui::BeginMenu("Language / Idioma")) {
            m_commands.menuItem("window.lang_en");
            m_commands.menuItem("window.lang_pt");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        m_commands.menuItem("window.reset_layout");
        m_commands.menuItem("window.legacy");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        m_commands.menuItem("help.palette");
        m_commands.menuItem("help.shortcuts");
        m_commands.menuItem("help.welcome");
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// Botão de alternância com dica (nome, atalho e explicação).
static bool toolButton(const char* label, bool active, const std::string& tip, bool enabled = true) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x * 0.7f, kAccent.y * 0.7f, kAccent.z * 0.7f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
    }
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label);
    ImGui::EndDisabled();
    if (active) ImGui::PopStyleColor(2);
    if (!tip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tip.c_str());
    return pressed;
}

static std::string commandTip(const EditorCommands& cmds, const char* id) {
    const EditorCommand* c = cmds.find(id);
    if (!c) return {};
    std::string tip = c->label;
    const std::string keys = cmds.shortcutText(*c);
    if (!keys.empty()) tip += "  (" + keys + ")";
    if (!c->help.empty()) tip += "\n" + c->help;
    return tip;
}

void Editor::drawToolbar() {
    const bool editing = m_play == PlayState::Editing;
    struct { const char* label; const char* id; Tool tool; } tools[] = {
        {"Select", "tool.select", Tool::Select},
        {"Move", "tool.move", Tool::Move},
        {"Rotate", "tool.rotate", Tool::Rotate},
        {"Scale", "tool.scale", Tool::Scale},
    };
    for (const auto& t : tools) {
        if (toolButton(t.label, m_tool == t.tool, commandTip(m_commands, t.id), editing)) m_tool = t.tool;
        ImGui::SameLine(0, 2);
    }
    ImGui::SameLine(0, 12);
    if (toolButton(m_localSpace ? "Local" : "World", false, commandTip(m_commands, "tool.space"), editing))
        m_localSpace = !m_localSpace;
    ImGui::SameLine(0, 2);
    if (toolButton("Snap", m_snap, commandTip(m_commands, "tool.snap"), editing)) m_snap = !m_snap;
    ImGui::SameLine(0, 2);
    ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 1.6f);
    if (ImGui::BeginCombo("##snapsteps", "", ImGuiComboFlags_NoPreview)) {
        ImGui::TextDisabled("Snap steps");
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Move", &m_snapMove, 0.05f, 0.01f, 100.0f, "%.2f");
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Rotate", &m_snapAngle, 0.5f, 1.0f, 90.0f, "%.0f deg");
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Scale", &m_snapScale, 0.01f, 0.01f, 10.0f, "%.2f");
        ImGui::EndCombo();
    }

    // Controles de jogo no centro.
    const float btn = ImGui::GetFrameHeight();
    const float groupW = btn * 3.0f + 8.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 16.0f, (ImGui::GetWindowWidth() - groupW) * 0.5f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto iconButton = [&](const char* id, bool active, bool enabled, const char* cmd, int icon) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x * 0.7f, kAccent.y * 0.7f, kAccent.z * 0.7f, 1.0f));
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::Button(id, ImVec2(btn, btn));
        ImGui::EndDisabled();
        if (active) ImGui::PopStyleColor();
        const std::string tip = commandTip(m_commands, cmd);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tip.c_str());
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const ImVec2 ctr((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const float r = btn * 0.26f;
        const ImU32 col = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        if (icon == 0) {        // play / stop
            if (m_play == PlayState::Editing)
                dl->AddTriangleFilled(ImVec2(ctr.x - r * 0.8f, ctr.y - r), ImVec2(ctr.x - r * 0.8f, ctr.y + r),
                                      ImVec2(ctr.x + r, ctr.y), col);
            else
                dl->AddRectFilled(ImVec2(ctr.x - r * 0.85f, ctr.y - r * 0.85f), ImVec2(ctr.x + r * 0.85f, ctr.y + r * 0.85f), col);
        } else if (icon == 1) { // pause
            dl->AddRectFilled(ImVec2(ctr.x - r * 0.8f, ctr.y - r), ImVec2(ctr.x - r * 0.25f, ctr.y + r), col);
            dl->AddRectFilled(ImVec2(ctr.x + r * 0.25f, ctr.y - r), ImVec2(ctr.x + r * 0.8f, ctr.y + r), col);
        } else {                // step
            dl->AddTriangleFilled(ImVec2(ctr.x - r, ctr.y - r), ImVec2(ctr.x - r, ctr.y + r), ImVec2(ctr.x + r * 0.4f, ctr.y), col);
            dl->AddRectFilled(ImVec2(ctr.x + r * 0.5f, ctr.y - r), ImVec2(ctr.x + r, ctr.y + r), col);
        }
        return pressed;
    };
    if (iconButton("##play", m_play != PlayState::Editing, true, "game.play", 0)) m_commands.run("game.play");
    ImGui::SameLine(0, 4);
    if (iconButton("##pause", m_play == PlayState::Paused, m_play != PlayState::Editing, "game.pause", 1)) togglePause();
    ImGui::SameLine(0, 4);
    if (iconButton("##step", false, m_play == PlayState::Paused, "game.step", 2)) step();

    // Velocidade da câmera à direita.
    const float speedW = 150.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, ImGui::GetWindowWidth() - speedW - 12.0f));
    ImGui::SetNextItemWidth(speedW);
    ImGui::DragFloat("##camspeed", &m_flySpeed, 0.5f, 1.0f, 2000.0f, "Fly speed %.0f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Fly speed. While flying (right mouse button held), the mouse wheel changes it.");
}

void Editor::drawStatusBar() {
    const char* hint = "";
    if (m_play == PlayState::Playing)
        hint = "Playing. F5 or Shift+F5 stops and restores the scene.";
    else if (m_flying)
        hint = "Flying: WASD move, Q/E down/up, Shift faster, wheel changes speed.";
    else if (m_viewportHovered)
        hint = "Right-drag: look/fly (WASD)   Middle-drag: pan   Alt+drag: orbit   Wheel: zoom   F: frame";
    else
        hint = "Ctrl+Shift+P: search any command   Ctrl+/: shortcuts";
    ImGui::TextDisabled("%s", hint);

    char scripts[96] = "";
    if (m_scripts) {
        if (m_scripts->failedCount() > 0)
            std::snprintf(scripts, sizeof(scripts), "Scripts: %zu (%zu with errors)   ", m_scripts->scriptCount(), m_scripts->failedCount());
        else if (m_scripts->running())
            std::snprintf(scripts, sizeof(scripts), "Scripts: %zu, %.2f ms   ", m_scripts->scriptCount(), m_scripts->lastUpdateMs());
        else
            std::snprintf(scripts, sizeof(scripts), "Scripts: %zu   ", m_scripts->scriptCount());
    }
    char right[320];
    const std::string& map = m_engine->currentMapName();
    std::snprintf(right, sizeof(right), "%s%s   %.0f FPS  %.2f ms", scripts, map.empty() ? "(no map)" : map.c_str(),
                  m_engine->fps(), m_engine->frameTime());
    const float w = ImGui::CalcTextSize(right).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, ImGui::GetWindowWidth() - w - 12.0f));
    ImGui::TextUnformatted(right);
}

void Editor::drawCommandPalette() {
    struct Entry {
        std::string text;
        std::string keys;
        bool enabled;
        std::function<void()> run;
    };
    std::vector<Entry> entries;
    for (const auto& c : m_commands.all()) {
        if (!c.run) continue;
        Entry e{c.category + ": " + c.label, m_commands.shortcutText(c), m_commands.isEnabled(c), nullptr};
        const std::string id = c.id;
        e.run = [this, id] { m_commands.run(id); };
        if (containsAllWords(e.text, m_paletteQuery)) entries.push_back(std::move(e));
    }
    for (const auto& name : m_engine->availableMaps()) {
        Entry e{"Map: " + name, {}, true, [this, name] { m_pendingMap = name; }};
        if (containsAllWords(e.text, m_paletteQuery)) entries.push_back(std::move(e));
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = std::min(620.0f, vp->WorkSize.x - 40.0f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - width) * 0.5f, vp->WorkPos.y + 40.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("##CommandPalette", nullptr, flags)) { ImGui::End(); return; }

    if (m_paletteFocus) {
        ImGui::SetKeyboardFocusHere();
        m_paletteFocus = false;
        m_paletteIndex = 0;
    }
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::InputTextWithHint("##query", "Type a command or a map name...", m_paletteQuery,
                                                  sizeof(m_paletteQuery));
    if (changed) m_paletteIndex = 0;
    const int count = static_cast<int>(entries.size());
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m_paletteIndex = std::min(count - 1, m_paletteIndex + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) m_paletteIndex = std::max(0, m_paletteIndex - 1);
    bool close = ImGui::IsKeyPressed(ImGuiKey_Escape);
    int runIndex = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) runIndex = m_paletteIndex;

    ImGui::BeginChild("##results", ImVec2(0, std::min(12, std::max(1, count)) * ImGui::GetTextLineHeightWithSpacing() + 8));
    if (count == 0) ImGui::TextDisabled("Nothing found.");
    for (int i = 0; i < count; ++i) {
        const Entry& e = entries[static_cast<size_t>(i)];
        ImGui::PushID(i);
        ImGui::BeginDisabled(!e.enabled);
        if (ImGui::Selectable(e.text.c_str(), i == m_paletteIndex)) runIndex = i;
        ImGui::EndDisabled();
        if (i == m_paletteIndex) ImGui::SetScrollHereY();
        if (!e.keys.empty()) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize(e.keys.c_str()).x - 4);
            ImGui::TextDisabled("%s", e.keys.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsWindowAppearing()) close = true;
    ImGui::End();

    if (runIndex >= 0 && runIndex < count && entries[static_cast<size_t>(runIndex)].enabled) {
        // O comando pode reabrir a paleta (Open Map): fecha antes de rodar.
        m_paletteOpen = false;
        entries[static_cast<size_t>(runIndex)].run();
        return;
    }
    if (close) m_paletteOpen = false;
}

void Editor::drawShortcutsWindow() {
    ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Keyboard Shortcuts", &m_showShortcuts, ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }
    std::string category;
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& c : m_commands.all()) {
            const std::string keys = m_commands.shortcutText(c);
            if (keys.empty()) continue;
            if (c.category != category) {
                category = c.category;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(kAccent, "%s", category.c_str());
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c.label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", keys.c_str());
        }
        const char* nav[][2] = {
            {"Look around / fly", "Hold right mouse + move"},
            {"Fly", "Right mouse + W A S D, Q/E down/up"},
            {"Fly faster", "Shift"},
            {"Fly speed", "Right mouse + wheel"},
            {"Pan", "Middle mouse drag"},
            {"Orbit", "Alt + left mouse drag"},
            {"Zoom", "Mouse wheel, Alt + right mouse drag"},
            {"Snap while dragging", "Hold Ctrl"},
        };
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(kAccent, "Scene navigation");
        for (const auto& n : nav) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(n[0]);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", n[1]);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Editor::play() {
    if (m_play != PlayState::Editing) return;
    const auto& insts = m_engine->modelRenderer().getInstances();
    m_snapshot.transforms.resize(insts.size());
    m_snapshot.enabled.resize(insts.size());
    for (size_t i = 0; i < insts.size(); ++i) {
        m_snapshot.transforms[i] = insts[i].transform;
        m_snapshot.enabled[i] = insts[i].enabled;
    }
    m_snapshot.lights = m_engine->m_deferredLighting.getPointLights();
    const Camera& cam = m_engine->camera();
    m_snapshot.target = cam.target();
    m_snapshot.yaw = cam.orbitYaw();
    m_snapshot.pitch = cam.orbitPitch();
    m_snapshot.distance = cam.orbitDistance();
    m_snapshot.timeOfDay = m_engine->dayNightCycle().timeOfDay();

    Camera& c = m_engine->camera();
    c.setMaxOrbitDistance(m_savedMaxDistance);
    c.setDebugFreeCamera(m_savedFreeCamera);
    m_flying = m_cameraDrag = false;
    m_play = PlayState::Playing;
    ImGui::SetWindowFocus("Scene###Scene");
    log(LogLevel::Info, "Play");
}

void Editor::stop() {
    if (m_play == PlayState::Editing) return;
    ModelRenderer& mr = m_engine->modelRenderer();
    const auto& insts = mr.getInstances();
    if (insts.size() == m_snapshot.transforms.size()) {
        for (size_t i = 0; i < insts.size(); ++i) {
            const uint32_t idx = static_cast<uint32_t>(i);
            if (insts[i].transform != m_snapshot.transforms[i]) mr.setInstanceTransform(idx, m_snapshot.transforms[i]);
            if (insts[i].enabled != m_snapshot.enabled[i]) mr.setInstanceEnabled(idx, m_snapshot.enabled[i]);
        }
    }
    auto& lights = m_engine->m_deferredLighting.getPointLights();
    if (lights.size() == m_snapshot.lights.size()) lights = m_snapshot.lights;
    m_engine->dayNightCycle().setTimeOfDay(m_snapshot.timeOfDay);

    Camera& cam = m_engine->camera();
    cam.setMaxOrbitDistance(20000.0f);
    cam.setDebugFreeCamera(true);
    cam.setOrbitTarget(m_snapshot.target);
    cam.setOrbit(m_snapshot.yaw, m_snapshot.pitch, m_snapshot.distance);
    m_play = PlayState::Editing;
    m_stepFrames = 0;
    log(LogLevel::Info, "Stop: scene restored");
}

void Editor::togglePause() {
    if (m_play == PlayState::Playing) m_play = PlayState::Paused;
    else if (m_play == PlayState::Paused) m_play = PlayState::Playing;
}

void Editor::step() {
    if (m_play == PlayState::Paused) m_stepFrames = 1;
}

bool Editor::openSourceReference(const std::string& text) {
    const size_t ext = text.find(".luau:");
    if (ext == std::string::npos) return false;
    size_t start = text.rfind(' ', ext);
    start = (start == std::string::npos) ? 0 : start + 1;
    const std::string file = text.substr(start, ext + 5 - start);
    const int line = std::atoi(text.c_str() + ext + 6);
    if (!std::filesystem::exists(file)) return false;
    openInExternalEditor(file, line);
    return true;
}

void Editor::openInExternalEditor(const std::filesystem::path& file, int line) {
    std::string target = file.string();
    if (line > 0) target += ":" + std::to_string(line);
    std::string quoted = "\"";
    for (char ch : target) {
        if (ch == '"' || ch == '\\' || ch == '$' || ch == '`') quoted += '\\';
        quoted += ch;
    }
    quoted += "\"";
#ifdef _WIN32
    const std::string cmd = "start \"\" code " + std::string(line > 0 ? "-g " : "") + quoted;
#else
    const bool hasCode = std::system("command -v code >/dev/null 2>&1") == 0;
    const std::string cmd = hasCode
        ? "code " + std::string(line > 0 ? "-g " : "") + quoted + " >/dev/null 2>&1 &"
        : "xdg-open \"" + file.string() + "\" >/dev/null 2>&1 &";
#endif
    if (std::system(cmd.c_str()) != 0) log(LogLevel::Error, "Could not open " + file.string());
    else log(LogLevel::Info, "Opened " + target);
}

} // namespace eruption
