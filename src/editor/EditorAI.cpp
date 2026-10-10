// Assistente de IA: conversa com a chave de API do usuário. O que a IA
// escreve em Luau passa pelo analisador antes de poder virar arquivo; nada
// é aplicado sozinho. Sem geração de imagem.
#include "editor/Editor.hpp"
#include "editor/Translate.hpp"

#include "editor/EditorTheme.hpp"

#include <Luau/Parser.h>
#include <imgui.h>

#include <fstream>
#include <regex>
#include <sstream>

namespace eruption {

namespace {

const char* kSystemIntro = R"(You help people make games with the Eruption Engine, a 3D engine with an editor.
Game logic is Luau code in assets/scripts/*.luau. Each file runs on its own and reloads when saved.
"Rule sheets" are the same Luau written as on.<event>(...) blocks with plain if/else inside; the editor shows
them as a sheet or as blocks, so prefer that shape for game rules.

Rules for your answers:
- Answer in the language the person writes in. Be brief.
- Use only the API declared below. Do not invent functions; if something is not possible with it, say so.
- When you write a script, put the whole file in ONE ```luau code block whose first line is
  `-- file: <name>.luau`. The editor checks the syntax and lets the person save it; nothing is applied by itself.
- Never add messages, keys, timers or behavior the person did not ask for.

Engine API (Luau declaration file):
)";

struct CodeBlock { std::string file, code; };

std::vector<CodeBlock> luauBlocks(const std::string& text) {
    std::vector<CodeBlock> out;
    static const std::regex fence("```(?:luau|lua)\\s*\\n([\\s\\S]*?)```");
    for (std::sregex_iterator it(text.begin(), text.end(), fence), end; it != end; ++it) {
        CodeBlock b;
        b.code = (*it)[1].str();
        std::smatch m;
        static const std::regex fileLine("^\\s*--\\s*file:\\s*([A-Za-z0-9_\\-/]+\\.luau)");
        if (std::regex_search(b.code, m, fileLine)) b.file = m[1].str();
        out.push_back(std::move(b));
    }
    return out;
}

// Primeiro erro de sintaxe (vazio = ok).
std::string syntaxError(const std::string& code) {
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    const Luau::ParseResult r = Luau::Parser::parse(code.data(), code.size(), names, allocator);
    if (r.errors.empty()) return {};
    const auto& e = r.errors.front();
    return "line " + std::to_string(e.getLocation().begin.line + 1) + ": " + e.getMessage();
}

} // namespace

void Editor::drawAiWindow() {
    if (!m_showAi) return;
    ImGui::SetNextWindowSize(ImVec2(640, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("AI Assistant###AIAssistant"), &m_showAi)) {
        ImGui::End();
        return;
    }
    if (m_ai.key.empty()) m_ai.key = AiClient::loadKey();
    if (m_ai.key.empty()) {
        ImGui::TextWrapped("%s", tr("The assistant uses your own Anthropic API key (console.anthropic.com). It is kept in "
                           "~/.config/eruption/anthropic_key, readable only by you, and never goes into the project. "
                           "You can also set ANTHROPIC_API_KEY."));
        ImGui::SetNextItemWidth(-90);
        ImGui::InputText("##key", m_ai.keyInput, sizeof(m_ai.keyInput), ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        if (ImGui::Button(tr("Save key")) && m_ai.keyInput[0]) {
            if (AiClient::saveKey(m_ai.keyInput)) m_ai.key = m_ai.keyInput;
            std::fill(std::begin(m_ai.keyInput), std::end(m_ai.keyInput), '\0');
        }
        ImGui::End();
        return;
    }
    if (!m_ai.promptReady) {
        // A declaracao de tipos vai copiada pro projeto; sem ela, a do motor.
        std::error_code declEc;
        const std::filesystem::path declInProject = m_projectRoot / "assets" / "scripts" / "eruption.d.luau";
        std::ifstream f(std::filesystem::exists(declInProject, declEc)
                            ? declInProject
                            : m_engineRoot / "assets" / "scripts" / "eruption.d.luau");
        std::ostringstream ss;
        ss << f.rdbuf();
        m_ai.client.setSystemPrompt(std::string(kSystemIntro) + ss.str());
        m_ai.promptReady = true;
    }
    char model[64];
    std::snprintf(model, sizeof(model), "%s", m_ai.client.model.c_str());
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText(tr("Model"), model, sizeof(model))) m_ai.client.model = model;
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("New conversation"))) m_ai.client.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Forget key"))) {
        AiClient::saveKey("");
        m_ai.key.clear();
    }
    ImGui::Separator();

    const float inputH = 90.0f;
    ImGui::BeginChild("##chat", ImVec2(0, -inputH), ImGuiChildFlags_Border);
    const auto turns = m_ai.client.turns();
    for (size_t t = 0; t < turns.size(); ++t) {
        const auto& turn = turns[t];
        ImGui::PushID(int(t));
        ImGui::TextColored(turn.user ? ImVec4(0.6f, 0.75f, 1.0f, 1.0f) : kAccent, "%s", turn.user ? "You" : "Assistant");
        if (turn.error) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", turn.text.c_str());
        else ImGui::TextWrapped("%s", turn.text.c_str());
        if (!turn.user && !turn.error) {
            const auto blocks = luauBlocks(turn.text);
            for (size_t b = 0; b < blocks.size(); ++b) {
                ImGui::PushID(int(b));
                const std::string err = syntaxError(blocks[b].code);
                const std::string name = blocks[b].file.empty() ? "ai_script_" + std::to_string(t) + ".luau" : blocks[b].file;
                if (err.empty()) ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.0f), tr("%s: syntax OK"), name.c_str());
                else ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), tr("%s: %s"), name.c_str(), err.c_str());
                ImGui::BeginDisabled(!err.empty());
                const auto path = m_projectRoot / "assets" / "scripts" / name;
                auto write = [&] {
                    std::error_code ec;
                    std::filesystem::create_directories(path.parent_path(), ec);
                    std::ofstream(path, std::ios::binary) << blocks[b].code;
                    log(LogLevel::Info, "Saved " + path.generic_string());
                };
                if (ImGui::Button(std::filesystem::exists(path) ? "Replace script" : "Save as script")) write();
                ImGui::SameLine();
                if (ImGui::Button(tr("Save and open in editor"))) {
                    write();
                    codeOpen(path);
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        ImGui::PopID();
    }
    if (m_ai.client.busy()) ImGui::TextDisabled("%s", tr("Thinking..."));
    if (turns.size() != m_ai.lastCount) {
        ImGui::SetScrollHereY(1.0f);
        m_ai.lastCount = turns.size();
    }
    ImGui::EndChild();

    ImGui::InputTextMultiline("##ask", m_ai.input, sizeof(m_ai.input), ImVec2(-90, inputH - 10));
    const bool ctrlEnter = ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_ai.client.busy() || !m_ai.input[0]);
    if ((ImGui::Button(tr("Send"), ImVec2(80, inputH - 10)) || ctrlEnter) && m_ai.input[0]) {
        if (m_ai.client.send(m_ai.key, m_ai.input)) m_ai.input[0] = '\0';
    }
    ImGui::EndDisabled();
    ImGui::End();
}

} // namespace eruption
