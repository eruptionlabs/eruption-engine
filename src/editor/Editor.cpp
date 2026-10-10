#include "editor/Editor.hpp"
#include "editor/Translate.hpp"
#include "editor/EditorTheme.hpp"
#include "game/PlayerController.hpp"

#include "core/Engine.hpp"
#include "script/ScriptHost.hpp"
#include "renderer/WeatherTypes.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

#include <algorithm>
#include <fstream>
#include <cctype>
#include <cstdio>
#include <cstring>
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
constexpr const char* kWinTelemetry = "Telemetry###Telemetry";


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

namespace {
// Nome comercial do processador (x86: instrução CPUID; outros: vazio).
std::string cpuName() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned regs[12] = {};
    if (__get_cpuid(0x80000000u, &regs[0], &regs[1], &regs[2], &regs[3]) && regs[0] >= 0x80000004u) {
        for (unsigned i = 0; i < 3; ++i)
            __get_cpuid(0x80000002u + i, &regs[i * 4], &regs[i * 4 + 1], &regs[i * 4 + 2], &regs[i * 4 + 3]);
        std::string name(reinterpret_cast<const char*>(regs), sizeof(regs));
        name.erase(name.find('\0') == std::string::npos ? name.size() : name.find('\0'));
        for (const char* junk : {"(R)", "(TM)", " CPU", "13th Gen ", "12th Gen ", "14th Gen "}) {
            for (size_t at; (at = name.find(junk)) != std::string::npos;) name.erase(at, std::strlen(junk));
        }
        const size_t first = name.find_first_not_of(' ');
        return first == std::string::npos ? std::string() : name.substr(first);
    }
#endif
    return {};
}
} // namespace

bool Editor::init(Engine& engine) {
    m_engine = &engine;
    // HUD e minimapa do jogo: só quando o jogo pede (ui.show_hud), no Play.
    engine.setHudBars(false);
    engine.setMinimapHud(false);
    m_hardware = engine.vulkan().gpuName();
    for (const char* junk : {"NVIDIA ", "GeForce ", "AMD ", "Radeon(TM) "}) {
        const size_t at = m_hardware.find(junk);
        if (at != std::string::npos) m_hardware.erase(at, std::strlen(junk));
    }
    const std::string cpu = cpuName();
    if (!cpu.empty()) m_hardware += "  |  " + cpu;
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

    initModelViewer();
    log(LogLevel::Info, "Editor ready. Press Ctrl+Shift+P to search every command.");
    return true;
}

void Editor::shutdown() {
    shutdownModelViewer();
    stopThumbnails();
    m_remote.stop();
    if (m_engine) {
        closeAssetPreview();
        for (auto& [path, t] : m_thumbs) releaseTexture(t.tex);
        m_thumbs.clear();
        releaseTexture(m_spriteImport.sheetTex);
        releaseTexture(m_mapgen.previewTex);
        m_engine->vulkan().waitIdle();
        collectTextures(true);
    }
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
    if (m_remote.running()) m_remote.pushLog(text);
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
    add("file.new_map", "File", "New Map...", {ImGuiKey_N, true}, [this] { m_newMapOpen = true; },
        "Creates an empty map (flat ground) in assets/external and opens it.")
        .enabled = isEditing;
    add("file.generate_map", "File", "Generate Map...", {}, [this] { m_showMapGen = true; },
        "Builds a map from a recipe of steps: hills, mountains, erosion, water, paint layers, objects.");
    add("file.open_map", "File", "Open Map...", {ImGuiKey_O, true}, [this] {
        m_paletteOpen = true;
        m_paletteFocus = true;
        std::snprintf(m_paletteQuery, sizeof(m_paletteQuery), "map ");
    }, "Search and load one of the available maps.");
    add("file.save", "File", "Save Scene", {ImGuiKey_S, true}, [this] { saveScene(); },
        "Writes what you changed in this map to assets/scenes/<map>.scene.luau. The game loads it too.")
        .enabled = [this] { return !m_engine->currentMapName().empty() && m_play == PlayState::Editing; };
    add("file.reload_scene", "File", "Reload Scene", {}, [this] { reloadSceneFile(); },
        "Discards unsaved changes and loads assets/scenes/<map>.scene.luau again.")
        .enabled = [this] { return !m_engine->currentMapName().empty() && m_play == PlayState::Editing; };
    add("file.export", "File", "Export Game...", {}, [this] { m_showExport = true; m_export.profileDetected = false; },
        "Copies the maps and only what they use into a folder that runs on its own.");
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
    add("edit.save_prefab", "Edit", "Save as Prefab...", {}, [this] { m_prefabOpen = true; },
        "Saves the selected object (model, rotation and scale) to assets/prefabs to place it again in any map.")
        .enabled = [this] { return m_play == PlayState::Editing && m_selection.kind == SelectionKind::Model; };
    add("edit.duplicate", "Edit", "Duplicate", {ImGuiKey_D, true}, [this] { duplicateSelection(); },
        "Copies the selected object next to it.")
        .enabled = [this] { return m_play == PlayState::Editing && m_selection.kind == SelectionKind::Model; };
    add("edit.snap_floor", "Edit", "Drop to Floor", {ImGuiKey_End}, [this] { snapSelectionToFloor(false); },
        "Puts the bottom of the selected object on the ground below it.")
        .enabled = [this] { return m_play == PlayState::Editing && selectionHasTransform(); };
    {
        Shortcut altEnd{ImGuiKey_End};
        altEnd.alt = true;
        add("edit.snap_pivot", "Edit", "Drop Pivot to Floor", altEnd, [this] { snapSelectionToFloor(true); },
            "Puts the origin point of the selected object on the ground (keeps sunk trunks and roots sunk).")
            .enabled = [this] { return m_play == PlayState::Editing && selectionHasTransform(); };
    }
    add("add.light", "Add", "Point Light", {}, [this] { addPointLight(); },
        "Adds a point light above the ground in the middle of the Scene view.")
        .enabled = isEditing;
    add("add.player_start", "Add", "Player Start", {}, [this] { setPlayerStartHere(); },
        "Puts the place where the player starts on the ground in the middle of the Scene view.")
        .enabled = isEditing;
    add("add.area", "Add", "Trigger Area", {}, [this] { addTriggerArea(); },
        "A named box on the ground: rules and scripts react when the player walks in or out.")
        .enabled = isEditing;
    add("add.sound", "Add", "Sound Emitter", {}, [this] { addSoundEmitter(); },
        "A sound that loops at a place (waterfall, campfire, tavern), heard when the player is near.")
        .enabled = isEditing;
    add("edit.delete", "Edit", "Delete", {ImGuiKey_Delete}, [this] { deleteSelection(); },
        "Hides the selected object (undo brings it back).")
        .enabled = [this] { return m_selection.kind == SelectionKind::Model || m_selection.kind == SelectionKind::Light; };
    {
        auto& c = add("edit.deselect", "Edit", "Deselect", {ImGuiKey_Escape}, [this] { select({}); });
        c.scope = Scope::Viewport;
        c.enabled = [this] { return m_selection.kind != SelectionKind::None; };
    }
    {
        {
            auto& iso = add("view.isolate", "View", "Isolate Selection", {ImGuiKey_Slash}, [this] { toggleIsolate(); },
                            "Shows only the selected models (the rest is hidden from the view, not deleted). Again: show all.");
            iso.scope = Scope::Viewport;
            iso.checked = [this] { return !m_isolated.empty(); };
            iso.enabled = [this] { return !m_isolated.empty() || m_selection.kind == SelectionKind::Model; };
            auto& full = add("view.full_detail", "View", "Full Detail (no LOD)", {},
                             [this] { m_engine->setFullDetailView(!m_engine->fullDetailView()); },
                             "Every model in its original mesh, near or far, on screen and in the shadow - to compare with the LOD.");
            full.checked = [this] { return m_engine->fullDetailView(); };
        }
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
    tool("tool.terrain", "Terrain", ImGuiKey_T, Tool::Terrain,
         "Sculpt the ground: left drag raises, Ctrl lowers, Shift smooths. Settings in the Inspector.");
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

    add("game.sprite_importer", "Game", "Sprite Importer", {}, [this] {
        m_showSpriteImporter = true;
        if (!m_projectSelected.empty() && std::filesystem::path(m_projectSelected).extension() == ".png")
            openSpriteImporter(m_projectSelected);
    }, "Cuts a PNG sprite sheet into standing and walking frames for 8 directions and saves a .spr next to it.");
    add("game.new_script", "Game", "New Script", {}, [this] {
        if (!m_scripts) return;
        const auto p = m_scripts->createScript("new_script");
        log(LogLevel::Info, "Created " + p.generic_string());
        m_projectDir = p.parent_path();
        m_projectSelected = p.string();
        codeOpen(p);
    }, "Creates a Luau script in the scripts folder and opens it.")
        .enabled = [this] { return m_scripts != nullptr; };
    add("game.install_debugger", "Game", "Install VS Code Debugger", {}, [this] {
        // A extensão (tools/vscode-eruption) vai para as extensões do VS Code do usuário.
        namespace fs = std::filesystem;
        const char* home = std::getenv("HOME");
        const fs::path src = m_projectRoot / "tools" / "vscode-eruption";
        if (!home || !fs::exists(src / "package.json")) {
            log(LogLevel::Error, "The debugger extension (tools/vscode-eruption) was not found.");
            return;
        }
        const fs::path dst = fs::path(home) / ".vscode" / "extensions" / "eruptionlabs.eruption-luau-debug-0.1.0";
        std::error_code ec;
        fs::create_directories(dst, ec);
        fs::copy(src, dst, fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
        if (ec) log(LogLevel::Error, "Could not install the debugger: " + ec.message());
        else log(LogLevel::Info, "Debugger installed in " + dst.string() + ". Restart VS Code, open assets/scripts and press F5 (Debug game scripts).");
    }, "Puts the Eruption debugger extension in VS Code: breakpoints and stepping in the game scripts.");
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
    {
        {
            auto& mv = add("window.model_viewer", "Window", "Model Viewer", {}, [this] {
                const bool open = !m_showModelViewer;
                // Abrindo com um modelo selecionado: ele já entra no visualizador.
                if (open && m_selection.kind == SelectionKind::Model && m_selection.index >= 0) {
                    const auto& inst = m_engine->modelRenderer().getInstances()[static_cast<size_t>(m_selection.index)];
                    if (!inst.assetPath.empty()) openModelViewer(inst.assetPath, static_cast<int>(inst.nodeIndex));
                }
                m_showModelViewer = open;
            }, "A model alone, with the engine's real rendering: LOD levels, wireframe, normals, UV and animations.");
            mv.checked = [this] { return m_showModelViewer; };
        }
        auto& ai = add("window.ai", "Window", "AI Assistant", {}, [this] { m_showAi = !m_showAi; },
                       "Chat that writes rules and scripts with your own API key. You review and save what it writes.");
        ai.checked = [this] { return m_showAi; };
        auto& r = add("window.second_screen", "Window", "Second Screen (browser)", {}, [this] { m_showRemote = !m_showRemote; },
                      "Edit the game scripts from a phone, tablet or another computer on the same network.");
        r.checked = [this] { return m_showRemote; };
        auto& c = add("window.telemetry", "Window", "Telemetry", {ImGuiKey_F3}, [this] { m_showTelemetry = !m_showTelemetry; },
                      "FPS, frame time, CPU and GPU cost per pass, video memory.");
        c.checked = [this] { return m_showTelemetry; };
    }
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
    // Painéis do próprio motor, como janelas encaixáveis do editor.
    auto engineTool = [&](const char* id, const char* label, Shortcut sc, bool* flag, const char* help) {
        auto& c = add(id, "Engine Tools", label, sc, [flag] { *flag = !*flag; }, help);
        c.checked = [flag] { return *flag; };
    };
    engineTool("engine.effects", "Effects, Lighting and Post", {ImGuiKey_F2}, &m_engine->m_showEffectsMenu,
               "Sun, ambient, fog, bloom, tone mapping, shadows, weather and every post effect.");
    engineTool("engine.telemetry", "Detailed Telemetry", {ImGuiKey_F3, false, true}, &m_engine->m_showStatsMenu,
               "The full engine telemetry: GPU per pass, memory, LOD, caches.");
    engineTool("engine.objects", "Object Manager", {}, &m_engine->m_showObjectMenu,
               "Model, light and texture lists of the engine.");
    engineTool("engine.player_light", "Player Light", {ImGuiKey_F10}, &m_engine->m_showPlayerLightMenu,
               "The light that follows the player.");
    engineTool("engine.ab", "A/B Test", {ImGuiKey_F9}, &m_engine->m_showABTestMenu,
               "Compare two settings side by side.");
    engineTool("engine.resources", "Resource Manager", {ImGuiKey_F12}, &m_engine->m_showResourceManager,
               "Textures, meshes and video memory in use.");

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
    // Painéis do motor (abertos pelo menu Engine Tools) nascem encaixados.
    ImGui::DockBuilderDockWindow("Iterative Effects (F2)", right);
    ImGui::DockBuilderDockWindow("Luz do Jogador (F10)", right);
    ImGui::DockBuilderDockWindow("Object Manager (F1)", right);
    ImGui::DockBuilderDockWindow("A/B Test (F9)", right);
    ImGui::DockBuilderDockWindow(kWinProject, bottom);
    ImGui::DockBuilderDockWindow(kWinConsole, bottom);
    ImGui::DockBuilderDockWindow(kWinTelemetry, bottom);
    ImGui::DockBuilderDockWindow("Telemetry (F3)", bottom);
    ImGui::DockBuilderDockWindow("Resource Manager (F12)", bottom);
    ImGui::DockBuilderFinish(dockspaceId);
    m_showHierarchy = m_showInspector = m_showProject = m_showConsole = m_showRules = m_showCode = m_showTelemetry = true;
    m_focusScene = true;
}

void Editor::update(float dt) {
    if (!m_engine) return;
    ++m_frame;
    setUiLanguage(m_lang); // a interface inteira segue o idioma escolhido
    recordFrameTime(dt);
    // Vizinho do mundo contínuo entrou ou saiu: camadas Mostrar/Travar valem para ele.
    if (m_engine->worldVersion() != m_worldVersionSeen) {
        m_worldVersionSeen = m_engine->worldVersion();
        applyLayerVisibility();
    }
    // ERUPTION_TEST_EDITOR_ASSET=caminho (teste): abre a prévia desse asset.
    static const char* testAsset = std::getenv("ERUPTION_TEST_EDITOR_ASSET");
    if (testAsset && m_frame == 30) selectAsset(testAsset);
    // ERUPTION_TEST_EDITOR_SELECT=trecho (teste/prints): seleciona e enquadra o
    // modelo mais perto da câmera cujo nome contém o trecho.
    static const char* testSelect = std::getenv("ERUPTION_TEST_EDITOR_SELECT");
    if (testSelect && m_frame == 120) {
        // Termina com "!": seleciona sem aproximar a câmera.
        std::string want = testSelect;
        const bool keepCamera = !want.empty() && want.back() == '!';
        if (keepCamera) want.pop_back();
        const auto& insts = m_engine->modelRenderer().getInstances();
        const Vec3 at = m_engine->camera().target();
        int best = -1;
        float bestD = 1e30f;
        for (size_t i = 0; i < insts.size(); ++i) {
            if (insts[i].name.find(want) == std::string::npos) continue;
            const float d = glm::length(insts[i].worldCenter - at);
            if (d < bestD) { bestD = d; best = static_cast<int>(i); }
        }
        if (best >= 0) {
            select({SelectionKind::Model, best});
            if (!keepCamera) frameSelection();
        }
    }
    // ERUPTION_TEST_EDITOR_NEWMAP=<nome>: cria o mapa plano e o abre (teste
    // do File > New Map sem digitar no diálogo).
    static const char* testNewMap = std::getenv("ERUPTION_TEST_EDITOR_NEWMAP");
    if (testNewMap && m_frame == 120) {
        std::string error;
        if (createFlatMap(testNewMap, 1000.0f, error)) m_pendingMap = testNewMap;
        else log(LogLevel::Error, error);
    }
    // ERUPTION_TEST_EDITOR_DROP=<arquivo.glb>@u,v: solta o modelo no ponto
    // (u,v em 0..1) da cena, como arrastar do painel Projeto.
    static const char* testDrop = std::getenv("ERUPTION_TEST_EDITOR_DROP");
    if (testDrop && m_frame == 200) {
        const std::string spec = testDrop;
        const size_t at = spec.rfind('@');
        float u = 0.5f, v = 0.5f;
        if (at != std::string::npos) std::sscanf(spec.c_str() + at + 1, "%f,%f", &u, &v);
        Vec3 p;
        const std::string file = spec.substr(0, at);
        if (groundPointAt(u, v, p)) {
            if (std::filesystem::path(file).extension() == ".png") dropCharacter(file, p);
            else dropModel(file, p);
        }
    }
    // ERUPTION_TEST_EDITOR_PREFAB=<nome>: gira 30 graus e escala 1,5 o objeto
    // selecionado, salva como prefab e solta uma cópia em (0.3, 0.6).
    static const char* testPrefab = std::getenv("ERUPTION_TEST_EDITOR_PREFAB");
    if (testPrefab && m_frame == 220 && m_selection.kind == SelectionKind::Model) {
        const uint32_t idx = static_cast<uint32_t>(m_selection.index);
        const Mat4 m = m_engine->modelRenderer().getInstances()[idx].transform;
        const Mat4 rs = glm::scale(glm::rotate(Mat4(1.0f), glm::radians(30.0f), Vec3(0, 1, 0)), Vec3(1.5f));
        m_engine->modelRenderer().setInstanceTransform(idx, glm::translate(Mat4(1.0f), Vec3(m[3])) * rs *
                                                                glm::translate(Mat4(1.0f), -Vec3(m[3])) * m);
        const std::string path = std::string("assets/prefabs/") + testPrefab + ".prefab.luau";
        Vec3 p;
        if (savePrefab(path) && groundPointAt(0.3f, 0.6f, p)) dropModel(path, p);
    }
    // ERUPTION_TEST_EDITOR_SPRITE=<folha.png>: abre o importador com a grade
    // padrão, grava o .spr e veste o jogador (sem gravar char_config.json).
    static const char* testSprite = std::getenv("ERUPTION_TEST_EDITOR_SPRITE");
    if (testSprite && m_frame == 120) {
        openSpriteImporter(testSprite);
        // ERUPTION_TEST_EDITOR_SPRITE_EXTRA=nome@linha: uma animação extra (uma vez).
        if (const char* extra = std::getenv("ERUPTION_TEST_EDITOR_SPRITE_EXTRA")) {
            const std::string spec = extra;
            const size_t at = spec.find('@');
            SpriteSheetSpec::NamedAnim e;
            e.name = spec.substr(0, at);
            e.anim = m_spriteImport.spec.walk;
            if (at != std::string::npos) e.anim.firstRow = std::atoi(spec.c_str() + at + 1);
            m_spriteImport.spec.extra.push_back(e);
        }
        std::string error;
        SpriteSheetResult res;
        if (buildSpriteSheet(m_spriteImport.spec, res, error) &&
            writeEruptSprV2(res, std::filesystem::path(testSprite).replace_extension(".spr").string(), error))
            usePlayerSprite(testSprite, false);
        else
            log(LogLevel::Error, error);
    }
    // ERUPTION_TEST_EDITOR_SCULPT=u,v,modo[,camada] (0 subir, 1 suavizar,
    // 2 aplainar, 3 baixar, 4 pintar a camada): um traço de 60 frames em (u,v), a
    // partir do frame 200, com o mesmo desfazer do mouse.
    // ERUPTION_TEST_EDITOR_LAYER=<png>: camada de terreno nova no frame 190.
    static const char* testLayer = std::getenv("ERUPTION_TEST_EDITOR_LAYER");
    if (testLayer && m_frame == 190) {
        if (m_engine->addSplatLayer(testLayer)) m_terrainDirty = true;
        else log(LogLevel::Error, std::string("Could not add terrain layer ") + testLayer);
    }
    // ERUPTION_TEST_EDITOR_SCATTER=<glb>: o modelo entra na paleta de objetos.
    static const char* testScatter = std::getenv("ERUPTION_TEST_EDITOR_SCATTER");
    if (testScatter && m_frame == 190) m_scatterPalette.push_back({testScatter, "", "", "test", true});
    static const char* testSculpt = std::getenv("ERUPTION_TEST_EDITOR_SCULPT");
    if (testSculpt && m_frame >= 200 && m_frame <= 260) {
        float u = 0.5f, v = 0.5f;
        int mode = 0, layer = 0;
        std::sscanf(testSculpt, "%f,%f,%d,%d", &u, &v, &mode, &layer);
        TerrainBrush& b = m_brush;
        Vec3 at;
        if (m_frame == 200 && groundPointAt(u, v, at)) {
            b.stroking = true;
            b.paint = mode == 4;
            b.objects = mode == 5;
            b.layer = layer;
            if (b.paint) m_engine->beginSplatEditing();
            if (!b.objects) b.snapshot = b.paint ? m_engine->splatEdit().weights : m_engine->heightEdit().delta;
            b.rect[0] = b.rect[1] = INT32_MAX;
            b.rect[2] = b.rect[3] = INT32_MIN;
            b.cursor = at;
            b.flattenTo = at.y;
        }
        if (b.stroking && m_frame < 260) {
            const Engine::SculptMode modes[] = {Engine::SculptMode::Raise, Engine::SculptMode::Smooth,
                                                Engine::SculptMode::Flatten, Engine::SculptMode::Lower};
            int r[4];
            if (b.objects) {
                b.size = 120.0f;
                scatterStep(1.0f / 60.0f, false);
            }
            const bool ok = b.objects ? false : b.paint ? m_engine->paintSplat(b.layer, Vec2(b.cursor.x, b.cursor.z), b.size * 2.0f, 4.0f, b.falloff,
                                                           1.0f / 60.0f, false, r)
                                    : m_engine->sculptTerrain(modes[std::clamp(mode, 0, 3)], Vec2(b.cursor.x, b.cursor.z),
                                                              b.size * 2.0f, b.strength * 2.0f, b.falloff, b.flattenTo,
                                                              1.0f / 60.0f, r);
            if (ok) {
                b.rect[0] = std::min(b.rect[0], r[0]);
                b.rect[1] = std::min(b.rect[1], r[1]);
                b.rect[2] = std::max(b.rect[2], r[2]);
                b.rect[3] = std::max(b.rect[3], r[3]);
            }
        } else if (b.stroking) {
            endTerrainStroke();
        }
    }
    // ERUPTION_TEST_EDITOR_WATER=<nível>: liga a água nesse nível no frame 265.
    static const char* testWater = std::getenv("ERUPTION_TEST_EDITOR_WATER");
    if (testWater && m_frame == 265) {
        m_engine->setWaterLevel(true, float(std::atof(testWater)));
        m_envEdited = m_waterEdited = true;
    }
    // ERUPTION_TEST_EDITOR_MAPGEN=<receita>|<mapa>: abre o gerador com a receita,
    // gera o mapa em segundo plano e o abre ao terminar.
    static const char* testMapGen = std::getenv("ERUPTION_TEST_EDITOR_MAPGEN");
    if (testMapGen && m_frame == 100) {
        const std::string spec = testMapGen;
        const size_t bar = spec.find('|');
        m_showMapGen = true;
        mapGenLoad(spec.substr(0, bar));
        m_mapgen.blocks = std::getenv("ERUPTION_TEST_EDITOR_MAPGEN_BLOCKS") != nullptr;
        if (m_mapgen.blocks) m_mapgen.selected = 0;
        if (bar != std::string::npos && bar + 1 < spec.size()) { // sem nome de mapa: só a prévia
            std::snprintf(m_mapgen.mapName, sizeof(m_mapgen.mapName), "%s", spec.substr(bar + 1).c_str());
            mapGenGenerate();
        }
    }
    // ERUPTION_TEST_EDITOR_EXPORT=<mapa>:<pasta>: exporta o mapa (síncrono).
    static const char* testExport = std::getenv("ERUPTION_TEST_EDITOR_EXPORT");
    if (testExport && m_frame == 100) {
        const std::string spec = testExport;
        ExportOptions o;
        o.projectRoot = m_projectRoot;
        o.maps = {spec.substr(0, spec.find(':'))};
        o.outDir = spec.substr(spec.find(':') + 1);
        std::error_code ec;
        o.binary = std::filesystem::exists(m_projectRoot / "build-export" / "eruption-engine", ec)
                       ? m_projectRoot / "build-export" / "eruption-engine"
                       : m_projectRoot / "eruption-engine";
        const ExportReport r = exportGame(o);
        ERUPTION_LOG_WARN("[EXPORT] files=%zu compiled=%zu neighbors=%zu bytes=%llu missing=%zu error=%s", r.files, r.compiled, r.neighbors,
                          static_cast<unsigned long long>(r.bytes), r.missing.size(), r.error.c_str());
        for (const auto& m : r.missing) ERUPTION_LOG_WARN("[EXPORT] missing %s", m.c_str());
    }
    // ERUPTION_TEST_EDITOR_AI=<chave>|<mensagem>: abre o assistente e envia
    // (teste do caminho do pedido; com chave falsa a API só recusa, sem custo).
    static const char* testAi = std::getenv("ERUPTION_TEST_EDITOR_AI");
    if (testAi && m_frame == 120) {
        const std::string spec = testAi;
        const size_t bar = spec.find('|');
        m_showAi = true;
        m_ai.key = spec.substr(0, bar);
        m_ai.promptReady = false;
        m_pendingAiMessage = spec.substr(bar + 1);
    }
    if (!m_pendingAiMessage.empty() && m_ai.promptReady) {
        m_ai.client.send(m_ai.key, m_pendingAiMessage);
        m_pendingAiMessage.clear();
    }
    // ERUPTION_TEST_EDITOR_REMOTE=<porta>:<arquivo>: liga a segunda tela e grava
    // o token no arquivo (teste com curl).
    static const char* testRemote = std::getenv("ERUPTION_TEST_EDITOR_REMOTE");
    if (testRemote && m_frame == 100) {
        const std::string spec = testRemote;
        const size_t colon = spec.find(':');
        if (m_remote.start(static_cast<uint16_t>(std::atoi(spec.c_str())), m_projectRoot / "assets" / "scripts"))
            std::ofstream(spec.substr(colon + 1)) << m_remote.token();
    }
    // ERUPTION_TEST_EDITOR_HIDE=<camada>: esconde a camada da vista (0 mapa, 1
    // adicionados, 2 pintados, 3 personagens, 4 marcadores) no frame 150.
    static const char* testHide = std::getenv("ERUPTION_TEST_EDITOR_HIDE");
    if (testHide && m_frame == 150) {
        m_layerShow[std::clamp(std::atoi(testHide), 0, int(LayerCount) - 1)] = false;
        applyLayerVisibility();
    }
    // ERUPTION_TEST_EDITOR_SLOPES=<graus>: bloqueia encostas mais íngremes no frame 270.
    static const char* testSlopes = std::getenv("ERUPTION_TEST_EDITOR_SLOPES");
    if (testSlopes && m_frame == 270) {
        log(LogLevel::Info, std::to_string(m_engine->blockSteepSlopes(float(std::atof(testSlopes)))) + " steep cells blocked");
        m_terrainDirty = true;
    }
    // ERUPTION_TEST_EDITOR_SAVE=<frame> (1 = frame 240): Ctrl+S depois dos ganchos acima.
    static const char* testSaveEnv = std::getenv("ERUPTION_TEST_EDITOR_SAVE");
    static const long testSaveFrame = testSaveEnv ? (std::atol(testSaveEnv) > 1 ? std::atol(testSaveEnv) : 240) : -1;
    if (static_cast<long>(m_frame) == testSaveFrame) saveScene();
    // ERUPTION_TEST_EDITOR_UNDO=<frame>: Ctrl+Z nesse frame.
    static const char* testUndo = std::getenv("ERUPTION_TEST_EDITOR_UNDO");
    if (testUndo && static_cast<long>(m_frame) == std::atol(testUndo)) m_undo.undo();
    // ERUPTION_TEST_EDITOR_EXTERNAL=<arquivo>: no frame 300 copia o arquivo por
    // cima do scene.luau do mapa, como um editor externo faria.
    static const char* testExternal = std::getenv("ERUPTION_TEST_EDITOR_EXTERNAL");
    if (testExternal && m_frame == 300) {
        std::error_code ec;
        std::filesystem::copy_file(testExternal, SceneEdits::pathFor(m_engine->currentMapName()),
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    // ERUPTION_TEST_EDITOR_PROBE=x,z: escreve no log a altura do chão pelos
    // modelos e pela grade .ter nesse ponto (diagnóstico de chão).
    static const char* testProbe = std::getenv("ERUPTION_TEST_EDITOR_PROBE");
    if (testProbe && m_frame == 150) {
        float x = 0.0f, z = 0.0f;
        std::sscanf(testProbe, "%f,%f", &x, &z);
        const Vec3 from(x, 5000.0f, z), down(0.0f, -1.0f, 0.0f);
        float tm = -1.0f, tt = -1.0f;
        const int hit = raycastModels(from, down, tm);
        const bool ter = m_engine->raycastTerrain(from, down, 50000.0f, tt);
        ERUPTION_LOG_WARN("[PROBE] (%.1f, %.1f): model floor %.2f (%s), terrain grid %.2f, sampleTerrainHeight %.2f", x, z,
                          hit >= 0 ? 5000.0f - tm : -1.0f,
                          hit >= 0 ? m_engine->modelRenderer().getInstances()[static_cast<size_t>(hit)].name.c_str() : "-",
                          ter ? 5000.0f - tt : -1.0f, m_engine->sampleTerrainHeight(x, z));
    }
    // ERUPTION_TEST_EDITOR_RAISE=<unidades>: levanta a seleção no frame 150.
    static const char* testRaise = std::getenv("ERUPTION_TEST_EDITOR_RAISE");
    if (testRaise && m_frame == 150 && selectionHasTransform()) {
        Mat4 m = selectionTransform();
        m[3].y += static_cast<float>(std::atof(testRaise));
        setSelectionTransform(m);
    }
    // ERUPTION_TEST_EDITOR_SHOW=aba[:arquivo] (prints): rules:<f>, code:<f>,
    // telemetry, palette:<texto>. ERUPTION_TEST_EDITOR_LANG=pt.
    static const char* testShow = std::getenv("ERUPTION_TEST_EDITOR_SHOW");
    if (testShow && m_frame == 160) {
        std::string all = testShow;
        size_t start = 0;
        while (start <= all.size()) {
            const size_t comma = all.find(',', start);
            const std::string spec = all.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            start = comma == std::string::npos ? all.size() + 1 : comma + 1;
            const size_t colon = spec.find(':');
            const std::string what = spec.substr(0, colon);
            const std::string arg = colon == std::string::npos ? std::string() : spec.substr(colon + 1);
            if (what == "rules") { rulesLoad(arg); m_pendingFocus.push_back(kWinRules); }
            else if (what == "blocks") { rulesLoad(arg); m_rules.blocks = true; m_pendingFocus.push_back(kWinRules); }
            else if (what == "code") { codeOpen(arg); m_pendingFocus.push_back(kWinCode); }
            else if (what == "scene") m_pendingFocus.push_back(kWinScene);
            else if (what == "env") select({SelectionKind::Environment, 0});
            else if (what == "area") select({SelectionKind::Area, m_engine->findArea(arg)});
            else if (what == "sound") select({SelectionKind::Sound, m_engine->findSoundEmitter(arg)});
            else if (what == "telemetry") m_pendingFocus.push_back(kWinTelemetry);
            else if (what == "project") {
                m_pendingFocus.push_back(kWinProject);
                if (!arg.empty()) m_projectDir = m_projectRoot / arg; // pasta aberta no painel
            }
            else if (what == "play") play();
            // Visualizador de modelo: viewer:<arquivo.glb>[#nó], viewer_sel (o selecionado),
            // viewer_lod:<0..3>, viewer_mode:<0..3>, viewer_wire, viewer_anim:<n>.
            else if (what == "viewer") {
                const size_t hash = arg.rfind('#');
                openModelViewer(arg.substr(0, hash), hash == std::string::npos ? -1 : std::atoi(arg.c_str() + hash + 1));
            }
            else if (what == "viewer_sel" && m_selection.kind == SelectionKind::Model && m_selection.index >= 0) {
                const auto& inst = m_engine->modelRenderer().getInstances()[static_cast<size_t>(m_selection.index)];
                openModelViewer(inst.assetPath, static_cast<int>(inst.nodeIndex));
            }
            else if (what == "viewer_lod") m_modelPreview.lod = static_cast<ModelPreview::Lod>(std::atoi(arg.c_str()) & 3);
            else if (what == "viewer_mode") m_modelPreview.mode = static_cast<ModelPreview::Mode>(std::atoi(arg.c_str()) & 3);
            else if (what == "viewer_wire") m_modelPreview.wireframe = true;
            else if (what == "viewer_anim") m_modelPreview.animation = std::atoi(arg.c_str());
            else if (what == "cmd") m_commands.run(arg);
            else if (what == "palette") {
                m_paletteOpen = true;
                m_paletteFocus = true;
                std::snprintf(m_paletteQuery, sizeof(m_paletteQuery), "%s", arg.c_str());
            }
        }
    }
    if (const char* lang = std::getenv("ERUPTION_TEST_EDITOR_LANG"); lang && m_frame == 2)
        m_lang = std::string(lang) == "pt" ? UiLanguage::Portuguese : UiLanguage::English;
    // ERUPTION_TEST_EDITOR_CLEAN=1 (prints): console vazio e sem cartão inicial.
    static const bool testClean = std::getenv("ERUPTION_TEST_EDITOR_CLEAN") != nullptr;
    if (testClean && m_frame == 150) {
        std::lock_guard<std::mutex> lock(m_consoleMutex);
        m_console.clear();
        m_consoleCounts[0] = m_consoleCounts[1] = m_consoleCounts[2] = 0;
        m_showWelcome = false;
    }
    uploadThumbnails();
    collectTextures(false);

    mapGenPoll();
    // Troca de mapa pedida pela UI: fora do frame de render.
    handleGameMapRequest();
    if (!m_pendingMap.empty()) {
        const std::string name = std::move(m_pendingMap);
        m_pendingMap.clear();
        if (m_play != PlayState::Editing) stop();
        select({});
        m_undo.clear();
        log(LogLevel::Info, "Loading map " + name + "...");
        m_engine->loadMap(name);
        m_lastMap.clear(); // reabrir o mesmo mapa também é mapa novo (cena, Play que continua)
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
        onMapReady();
    }

    checkSceneDirty();
    watchSceneFile();
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
    if (m_showTelemetry) drawTelemetry();
    drawViewport();
    if (m_showHierarchy) drawHierarchy();
    if (m_showInspector) drawInspector();
    if (m_showProject) drawProject();
    if (m_showConsole) drawConsole();
    if (m_showShortcuts) drawShortcutsWindow();
    if (m_paletteOpen) drawCommandPalette();
    drawNewMapDialog();
    drawPrefabDialog();
    drawSpriteImporter();
    drawRemoteWindow();
    drawAiWindow();
    drawExportWindow();
    drawMapGenWindow();
    drawModelViewer();

    if (m_focusScene) {
        m_pendingFocus.insert(m_pendingFocus.begin(), kWinScene);
        m_focusScene = false;
    }
    // Traz a aba para a frente no nó onde ela está encaixada.
    for (const char* name : m_pendingFocus) {
        if (ImGuiWindow* w = ImGui::FindWindowByName(name)) {
            if (w->DockNode && w->DockNode->TabBar) w->DockNode->TabBar->NextSelectedTabId = w->TabId;
            ImGui::FocusWindow(w);
        }
    }
    m_pendingFocus.clear();

    m_commands.dispatchShortcuts((m_viewportHovered || m_viewportFocused) && m_play != PlayState::Playing);
}

void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu(tr("File"))) {
        m_commands.menuItem("file.new_map");
        m_commands.menuItem("file.generate_map");
        m_commands.menuItem("file.open_map");
        m_commands.menuItem("file.save");
        m_commands.menuItem("file.reload_scene");
        if (ImGui::BeginMenu(tr("Recent Maps"), !m_engine->availableMaps().empty())) {
            for (const auto& name : m_engine->availableMaps())
                if (ImGui::MenuItem(name.c_str(), nullptr, name == m_engine->currentMapName()))
                    m_pendingMap = name;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        m_commands.menuItem("file.export");
        m_commands.menuItem("file.open_vscode");
        ImGui::Separator();
        m_commands.menuItem("file.quit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Edit"))) {
        const EditorCommand* u = m_commands.find("edit.undo");
        const EditorCommand* r = m_commands.find("edit.redo");
        const std::string ul = m_undo.canUndo() ? "Undo " + m_undo.undoLabel() : "Undo";
        const std::string rl = m_undo.canRedo() ? "Redo " + m_undo.redoLabel() : "Redo";
        if (ImGui::MenuItem(ul.c_str(), EditorCommands::shortcutText(u->shortcut).c_str(), false, m_undo.canUndo()))
            m_commands.run("edit.undo");
        if (ImGui::MenuItem(rl.c_str(), EditorCommands::shortcutText(r->shortcut).c_str(), false, m_undo.canRedo()))
            m_commands.run("edit.redo");
        ImGui::Separator();
        m_commands.menuItem("edit.duplicate");
        m_commands.menuItem("edit.snap_floor");
        m_commands.menuItem("edit.snap_pivot");
        m_commands.menuItem("edit.save_prefab");
        m_commands.menuItem("edit.delete");
        m_commands.menuItem("edit.deselect");
        m_commands.menuItem("edit.frame");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("View"))) {
        static const char* kNames[LayerCount] = {"Map objects", "Added objects", "Painted objects", "Characters",
                                                 "Lights and markers", "Neighbor maps"};
        ImGui::TextDisabled("%s", tr("Show"));
        bool changed = false;
        for (int l = 0; l < LayerCount; ++l) changed |= ImGui::MenuItem(kNames[l], nullptr, &m_layerShow[l]);
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Lock (can't be clicked)"));
        for (int l = 0; l < LayerCount; ++l) {
            ImGui::PushID(l);
            ImGui::MenuItem(kNames[l], nullptr, &m_layerLock[l]);
            ImGui::PopID();
        }
        if (changed) applyLayerVisibility();
        ImGui::Separator();
        m_commands.menuItem("view.isolate");
        m_commands.menuItem("view.full_detail");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Add"))) {
        m_commands.menuItem("add.light");
        m_commands.menuItem("add.player_start");
        m_commands.menuItem("add.area");
        m_commands.menuItem("add.sound");
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Models, prefabs and characters:"));
        ImGui::TextDisabled("%s", tr("drag them from the Project panel"));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Tools"))) {
        m_commands.menuItem("tool.select");
        m_commands.menuItem("tool.move");
        m_commands.menuItem("tool.rotate");
        m_commands.menuItem("tool.scale");
        m_commands.menuItem("tool.terrain");
        ImGui::Separator();
        m_commands.menuItem("tool.space");
        m_commands.menuItem("tool.snap");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Game"))) {
        m_commands.menuItem("game.play");
        m_commands.menuItem("game.stop");
        m_commands.menuItem("game.pause");
        m_commands.menuItem("game.step");
        ImGui::Separator();
        m_commands.menuItem("game.new_script");
        m_commands.menuItem("game.install_debugger");
        m_commands.menuItem("game.open_scripts");
        ImGui::Separator();
        m_commands.menuItem("game.sprite_importer");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Window"))) {
        m_commands.menuItem("window.hierarchy");
        m_commands.menuItem("window.inspector");
        m_commands.menuItem("window.project");
        m_commands.menuItem("window.console");
        m_commands.menuItem("window.rules");
        m_commands.menuItem("window.code");
        m_commands.menuItem("window.telemetry");
        m_commands.menuItem("window.second_screen");
        m_commands.menuItem("window.ai");
        m_commands.menuItem("window.model_viewer");
        ImGui::Separator();
        if (ImGui::BeginMenu(tr("Language / Idioma"))) {
            m_commands.menuItem("window.lang_en");
            m_commands.menuItem("window.lang_pt");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        m_commands.menuItem("window.reset_layout");
        if (ImGui::BeginMenu(tr("Engine Tools"))) {
            for (const char* id : {"engine.effects", "engine.telemetry", "engine.objects", "engine.player_light",
                                   "engine.ab", "engine.resources"})
                m_commands.menuItem(id);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Help"))) {
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
    std::string tip = tr(c->label.c_str());
    const std::string keys = cmds.shortcutText(*c);
    if (!keys.empty()) tip += "  (" + keys + ")";
    if (!c->help.empty()) tip += std::string("\n") + tr(c->help.c_str());
    return tip;
}

void Editor::drawToolbar() {
    const bool editing = m_play == PlayState::Editing;
    struct { const char* label; const char* id; Tool tool; } tools[] = {
        {"Select", "tool.select", Tool::Select},
        {"Move", "tool.move", Tool::Move},
        {"Rotate", "tool.rotate", Tool::Rotate},
        {"Scale", "tool.scale", Tool::Scale},
        {"Terrain", "tool.terrain", Tool::Terrain},
    };
    for (const auto& t : tools) {
        if (toolButton(tr(t.label), m_tool == t.tool, commandTip(m_commands, t.id), editing)) m_tool = t.tool;
        ImGui::SameLine(0, 2);
    }
    ImGui::SameLine(0, 12);
    if (toolButton(tr(m_localSpace ? "Local" : "World"), false, commandTip(m_commands, "tool.space"), editing))
        m_localSpace = !m_localSpace;
    ImGui::SameLine(0, 2);
    if (toolButton(tr("Snap"), m_snap, commandTip(m_commands, "tool.snap"), editing)) m_snap = !m_snap;
    ImGui::SameLine(0, 2);
    ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 1.6f);
    if (ImGui::BeginCombo("##snapsteps", "", ImGuiComboFlags_NoPreview)) {
        ImGui::TextDisabled("%s", tr("Snap steps"));
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat(tr("Move"), &m_snapMove, 0.05f, 0.01f, 100.0f, "%.2f");
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat(tr("Rotate"), &m_snapAngle, 0.5f, 1.0f, 90.0f, "%.0f deg");
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat(tr("Scale"), &m_snapScale, 0.01f, 0.01f, 10.0f, "%.2f");
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
    ImGui::DragFloat("##camspeed", &m_flySpeed, 0.5f, 1.0f, 2000.0f, tr("Fly speed %.0f"), ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", tr("Fly speed. While flying (right mouse button held), the mouse wheel changes it."));
}

void Editor::drawStatusBar() {
    const char* hint = "";
    if (m_play == PlayState::Playing)
        hint = tr("Playing. F5 or Shift+F5 stops and restores the scene.");
    else if (m_flying)
        hint = tr("Flying: WASD move, Q/E down/up, Shift faster, wheel changes speed.");
    else if (m_viewportHovered)
        hint = tr("Right-drag: look/fly (WASD)   Middle-drag: pan   Alt+drag: orbit   Wheel: zoom   F: frame");
    else
        hint = tr("Ctrl+Shift+P: search any command   Ctrl+/: shortcuts");
    ImGui::TextDisabled("%s", hint);

    char scripts[96] = "";
    if (m_scripts) {
        if (m_scripts->failedCount() > 0)
            std::snprintf(scripts, sizeof(scripts), tr("Scripts: %zu (%zu with errors)   "), m_scripts->scriptCount(), m_scripts->failedCount());
        else if (m_scripts->running())
            std::snprintf(scripts, sizeof(scripts), tr("Scripts: %zu, %.2f ms   "), m_scripts->scriptCount(), m_scripts->lastUpdateMs());
        else
            std::snprintf(scripts, sizeof(scripts), tr("Scripts: %zu   "), m_scripts->scriptCount());
    }
    char right[320];
    const std::string& map = m_engine->currentMapName();
    std::snprintf(right, sizeof(right), "%s%s   %.0f FPS  %.2f ms   %s", scripts, map.empty() ? "(no map)" : map.c_str(),
                  m_engine->fps(), m_engine->frameTime(), m_hardware.c_str());
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
        // Procura nos dois idiomas (o traduzido aparece, o inglês também acha).
        Entry e{std::string(tr(c.category.c_str())) + ": " + tr(c.label.c_str()), m_commands.shortcutText(c),
                m_commands.isEnabled(c), nullptr};
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
    const bool changed = ImGui::InputTextWithHint("##query", tr("Type a command or a map name..."), m_paletteQuery,
                                                  sizeof(m_paletteQuery));
    if (changed) m_paletteIndex = 0;
    const int count = static_cast<int>(entries.size());
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m_paletteIndex = std::min(count - 1, m_paletteIndex + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) m_paletteIndex = std::max(0, m_paletteIndex - 1);
    bool close = ImGui::IsKeyPressed(ImGuiKey_Escape);
    int runIndex = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) runIndex = m_paletteIndex;

    ImGui::BeginChild("##results", ImVec2(0, std::min(12, std::max(1, count)) * ImGui::GetTextLineHeightWithSpacing() + 8));
    if (count == 0) ImGui::TextDisabled("%s", tr("Nothing found."));
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
    if (!ImGui::Begin(trw("Keyboard Shortcuts"), &m_showShortcuts, ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }
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
            ImGui::TextUnformatted(tr(c.label.c_str()));
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
        ImGui::TextColored(kAccent, "%s", tr("Scene navigation"));
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

void Editor::handleGameMapRequest() {
    Engine::MapRequest request;
    if (m_engine->takeMapRequest(request)) {
        if (m_play == PlayState::Editing) return; // só o jogo troca de mapa
        m_playResume = {};
        m_playResume.active = true;
        m_playResume.hasSpawn = request.hasSpawn;
        m_playResume.spawn = request.spawn;
        if (m_scripts) m_playResume.vars = m_scripts->exportVars();
        m_playResume.readyAt = m_mapReadyCount;
        m_pendingMap = request.map; // para o Play e abre
        log(LogLevel::Info, "The game went to map " + request.map);
        return;
    }
    if (!m_playResume.active || m_engine->currentMapName().empty() || !m_engine->isPlayerSpawnedOnActiveMap()) return;
    if (!m_playResume.started) {
        // Mapa aberto e cena aplicada (onMapReady): volta a jogar.
        if (m_play == PlayState::Editing && m_mapReadyCount != m_playResume.readyAt) {
            if (m_scripts) m_scripts->resumeOnNextStart(m_playResume.vars);
            play();
            m_playResume.started = true;
        }
        return;
    }
    if (m_scripts && !m_scripts->running()) return; // os scripts começam no próximo frame
    if (m_playResume.hasSpawn && m_engine->m_playerController) {
        const Vec3 p(m_playResume.spawn.x, m_engine->sampleTerrainHeight(m_playResume.spawn.x, m_playResume.spawn.z),
                     m_playResume.spawn.z);
        m_engine->m_playerController->setPos(p);
        m_engine->camera().setOrbitTarget(p);
    }
    m_playResume = {};
}

void Editor::play() {
    if (m_play != PlayState::Editing) return;
    m_engine->setSoundEmittersActive(true); // sons da cena tocam no Play
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
    m_snapshot.characters.clear();
    for (const auto& c : m_engine->characters()) m_snapshot.characters.push_back({c.position, c.facing, c.visible, c.sprite});
    m_snapshot.animations.clear();
    for (uint32_t i = 0; i < insts.size(); ++i)
        if (m_engine->modelRenderer().isSkinned(i)) m_snapshot.animations.push_back({i, m_engine->modelRenderer().currentAnimation(i)});
    m_snapshot.playerSprite = m_engine->m_charConfig.job;
    // O jogo começa no início do jogador quando o mapa tem um.
    if (m_engine->hasPlayerStart() && m_engine->m_playerController)
        m_engine->m_playerController->setPos(m_engine->playerStart());

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
    m_engine->setSoundEmittersActive(false);
    if (m_scripts && !m_playResume.active) m_scripts->clearMapStates(); // Stop de verdade: próximo Play do zero
    m_engine->setHudBars(false); // o que o jogo ligou vale só no Play
    m_engine->setMinimapHud(false);
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
    for (const auto& [i, anim] : m_snapshot.animations)
        if (m_engine->modelRenderer().currentAnimation(i) != anim) m_engine->modelRenderer().playAnimation(i, anim);
    m_engine->dayNightCycle().setTimeOfDay(m_snapshot.timeOfDay);
    auto& chars = m_engine->characters();
    for (size_t i = 0; i < chars.size() && i < m_snapshot.characters.size(); ++i) {
        const auto& snap = m_snapshot.characters[i];
        chars[i].position = snap.position;
        chars[i].facing = snap.facing;
        chars[i].visible = snap.visible;
        chars[i].walking = false;
        chars[i].behavior = Engine::SceneCharacter::Behavior::None; // seguir/patrulhar vale só no Play
        chars[i].patrol.clear();
        if (chars[i].sprite != snap.sprite) m_engine->setCharacterSprite(static_cast<int>(i), snap.sprite);
    }
    if (m_engine->m_charConfig.job != m_snapshot.playerSprite) {
        m_engine->m_charConfig.job = m_snapshot.playerSprite;
        if (m_engine->m_playerController) m_engine->m_playerController->reloadSprite();
    }

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
