// Painel de código: edição de scripts .luau dentro do editor, com cores de
// sintaxe, verificação de erros enquanto se digita e Ctrl+S.
#include "editor/Editor.hpp"
#include "editor/EditorTheme.hpp"

#include <Luau/Parser.h>
#include <TextEditor.h>
#include <imgui.h>

#include <fstream>
#include <sstream>

namespace eruption {

namespace {

namespace fs = std::filesystem;

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const TextEditor::LanguageDefinition& luauLanguage() {
    static TextEditor::LanguageDefinition def = [] {
        TextEditor::LanguageDefinition d = TextEditor::LanguageDefinition::Lua();
        d.mName = "Luau";
        for (const char* k : {"continue", "export", "type", "typeof"}) d.mKeywords.insert(k);
        static const struct { const char* name; const char* doc; } kEngine[] = {
            {"on", "Events: on.start, on.update, on.every, on.key, on.near, on.hour"},
            {"vars", "Shared variables: vars.get, vars.set, vars.add"},
            {"ui", "Screen: ui.message(text, seconds)"},
            {"world", "Time of day and weather"},
            {"player", "Player position"},
            {"camera", "Camera target and orbit"},
            {"input", "Keyboard: input.down, input.pressed"},
            {"models", "Objects of the map, by name or index"},
            {"lights", "Point lights of the map, by index"},
            {"log", "Prints to the editor console"},
            {"vector", "vector.create(x, y, z)"},
        };
        for (const auto& e : kEngine) {
            TextEditor::Identifier id;
            id.mDeclaration = e.doc;
            d.mIdentifiers.insert({e.name, id});
        }
        return d;
    }();
    return def;
}

TextEditor::Palette editorPalette() {
    TextEditor::Palette p = TextEditor::GetDarkPalette();
    auto set = [&](TextEditor::PaletteIndex i, ImU32 c) { p[static_cast<unsigned>(i)] = c; };
    set(TextEditor::PaletteIndex::Background, IM_COL32(27, 28, 29, 255));
    set(TextEditor::PaletteIndex::Keyword, IM_COL32(255, 120, 90, 255));
    set(TextEditor::PaletteIndex::KnownIdentifier, IM_COL32(110, 190, 90, 255));
    set(TextEditor::PaletteIndex::Number, IM_COL32(120, 175, 245, 255));
    set(TextEditor::PaletteIndex::String, IM_COL32(230, 200, 120, 255));
    set(TextEditor::PaletteIndex::Comment, IM_COL32(120, 125, 130, 255));
    set(TextEditor::PaletteIndex::MultiLineComment, IM_COL32(120, 125, 130, 255));
    set(TextEditor::PaletteIndex::LineNumber, IM_COL32(95, 98, 104, 255));
    set(TextEditor::PaletteIndex::CurrentLineFill, IM_COL32(255, 255, 255, 10));
    set(TextEditor::PaletteIndex::CurrentLineFillInactive, IM_COL32(255, 255, 255, 6));
    set(TextEditor::PaletteIndex::CurrentLineEdge, IM_COL32(255, 255, 255, 20));
    set(TextEditor::PaletteIndex::Selection, IM_COL32(255, 60, 40, 80));
    return p;
}

} // namespace

Editor::Editor() = default;
Editor::~Editor() = default;

void Editor::codeOpen(const fs::path& file) {
    if (!m_code.editor) {
        m_code.editor = std::make_unique<TextEditor>();
        m_code.editor->SetLanguageDefinition(luauLanguage());
        m_code.editor->SetPalette(editorPalette());
        m_code.editor->SetShowWhitespaces(false);
    }
    m_code.file = file;
    m_code.saved = readText(file);
    std::error_code ec;
    m_code.mtime = fs::last_write_time(file, ec);
    m_code.editor->SetText(m_code.saved);
    m_code.checkTimer = 0.0f;
    m_code.checked.clear();
    m_showCode = true;
    m_code.focus = true;
}

bool Editor::codeSave() {
    if (!m_code.editor || m_code.file.empty()) return false;
    std::string text = m_code.editor->GetText();
    // O editor sempre acrescenta uma linha vazia ao fim; não duplica.
    if (!text.empty() && text.back() == '\n' && !m_code.saved.empty() && m_code.saved.back() == '\n' &&
        text.size() >= 2 && text[text.size() - 2] == '\n')
        text.pop_back();
    std::ofstream(m_code.file, std::ios::binary) << text;
    m_code.saved = text;
    std::error_code ec;
    m_code.mtime = fs::last_write_time(m_code.file, ec);
    log(LogLevel::Info, "Saved " + m_code.file.filename().string());
    if (m_rules.file == m_code.file) rulesLoad(m_rules.file);
    return true;
}

void Editor::drawCode() {
    const bool dirty = m_code.editor && m_code.editor->GetText() != m_code.saved &&
                       m_code.editor->GetText() != m_code.saved + "\n";
    const std::string title = std::string("Code") + (dirty ? " *" : "") + "###Code";
    if (m_code.focus) {
        ImGui::SetNextWindowFocus();
        m_code.focus = false;
    }
    if (!ImGui::Begin(title.c_str(), &m_showCode)) { ImGui::End(); return; }

    if (!m_code.editor || m_code.file.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Double-click a .luau file in the Project panel to edit it here.");
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }

    // Mudou fora (VS Code, IA, folha de eventos) e aqui não há edição pendente.
    std::error_code ec;
    if (!dirty && fs::exists(m_code.file, ec) && fs::last_write_time(m_code.file, ec) != m_code.mtime) codeOpen(m_code.file);

    if (ImGui::Button("Save")) codeSave();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Ctrl+S. The game reloads the script on its own.");
    ImGui::SameLine();
    if (ImGui::Button("Open in VS Code")) {
        const auto pos = m_code.editor->GetCursorPosition();
        openInExternalEditor(m_code.file, pos.mLine + 1);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_code.file.generic_string().c_str());
    const auto cur = m_code.editor->GetCursorPosition();
    char lc[48];
    std::snprintf(lc, sizeof(lc), "Ln %d, Col %d", cur.mLine + 1, cur.mColumn + 1);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(lc).x));
    ImGui::TextDisabled("%s", lc);

    // Erros de sintaxe enquanto se digita (o mesmo analisador do Luau).
    m_code.checkTimer += ImGui::GetIO().DeltaTime;
    if (m_code.checkTimer > 0.3f) {
        m_code.checkTimer = 0.0f;
        const std::string text = m_code.editor->GetText();
        if (text != m_code.checked) {
            m_code.checked = text;
            Luau::Allocator allocator;
            Luau::AstNameTable names(allocator);
            const Luau::ParseResult r = Luau::Parser::parse(text.data(), text.size(), names, allocator);
            TextEditor::ErrorMarkers markers;
            for (const auto& e : r.errors) markers[static_cast<int>(e.getLocation().begin.line) + 1] = e.getMessage();
            m_code.editor->SetErrorMarkers(markers);
            m_code.errors = static_cast<int>(r.errors.size());
        }
    }
    if (m_code.errors > 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.38f, 0.35f, 1.0f), "%d syntax error(s): hover the red line to read it.", m_code.errors);
    }

    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) codeSave();
    m_code.editor->Render("##code");
    ImGui::End();
}

} // namespace eruption
