#include "editor/EditorCommands.hpp"

namespace eruption {

EditorCommand& EditorCommands::add(EditorCommand cmd) {
    m_commands.push_back(std::move(cmd));
    return m_commands.back();
}

const EditorCommand* EditorCommands::find(const std::string& id) const {
    for (const auto& c : m_commands)
        if (c.id == id) return &c;
    return nullptr;
}

bool EditorCommands::isEnabled(const EditorCommand& cmd) const {
    return cmd.run && (!cmd.enabled || cmd.enabled());
}

bool EditorCommands::run(const std::string& id) {
    const EditorCommand* c = find(id);
    if (!c || !isEnabled(*c)) return false;
    c->run();
    return true;
}

static bool shortcutPressed(const Shortcut& s) {
    if (!s.valid()) return false;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl != s.ctrl || io.KeyShift != s.shift || io.KeyAlt != s.alt) return false;
    return ImGui::IsKeyPressed(s.key, false);
}

void EditorCommands::dispatchShortcuts(bool viewportActive) {
    const ImGuiIO& io = ImGui::GetIO();
    // Digitando num campo de texto as teclas pertencem ao campo.
    if (io.WantTextInput) return;
    for (const auto& c : m_commands) {
        if (c.scope == EditorCommand::Scope::Viewport && !viewportActive) continue;
        if (!shortcutPressed(c.shortcut) && !shortcutPressed(c.altShortcut)) continue;
        if (isEnabled(c)) c.run();
        return;
    }
}

void EditorCommands::menuItem(const std::string& id) {
    const EditorCommand* c = find(id);
    if (!c) return;
    const std::string keys = shortcutText(*c);
    const bool checked = c->checked && c->checked();
    if (ImGui::MenuItem(c->label.c_str(), keys.empty() ? nullptr : keys.c_str(), checked, isEnabled(*c)))
        c->run();
    if (!c->help.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", c->help.c_str());
}

std::string EditorCommands::shortcutText(const Shortcut& s) {
    if (!s.valid()) return {};
    std::string out;
    if (s.ctrl) out += "Ctrl+";
    if (s.shift) out += "Shift+";
    if (s.alt) out += "Alt+";
    out += ImGui::GetKeyName(s.key);
    return out;
}

std::string EditorCommands::shortcutText(const EditorCommand& cmd) const {
    std::string a = shortcutText(cmd.shortcut);
    const std::string b = shortcutText(cmd.altShortcut);
    if (!b.empty()) a += a.empty() ? b : "  /  " + b;
    return a;
}

} // namespace eruption
