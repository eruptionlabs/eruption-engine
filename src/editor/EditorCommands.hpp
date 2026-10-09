#pragma once

#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

namespace eruption {

struct Shortcut {
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false, shift = false, alt = false;
    bool valid() const { return key != ImGuiKey_None; }
};

// Toda ação do editor mora aqui, uma vez: o menu, a paleta de comandos, a
// janela de atalhos e o teclado leem a mesma lista. Assim nome, atalho e
// estado (ativo, marcado) nunca divergem entre um lugar e outro.
struct EditorCommand {
    enum class Scope { Global, Viewport };
    std::string id;
    std::string label;
    std::string category;
    std::string help;
    Shortcut shortcut;
    Shortcut altShortcut;
    Scope scope = Scope::Global;
    std::function<void()> run;
    std::function<bool()> enabled;
    std::function<bool()> checked;
};

class EditorCommands {
public:
    EditorCommand& add(EditorCommand cmd);
    const EditorCommand* find(const std::string& id) const;
    const std::vector<EditorCommand>& all() const { return m_commands; }

    bool isEnabled(const EditorCommand& cmd) const;
    bool run(const std::string& id);
    // Atalhos do frame. `viewportActive`: o mouse está sobre a cena ou ela
    // tem o foco; só então valem os atalhos de escopo Viewport.
    void dispatchShortcuts(bool viewportActive);
    // Item de menu ligado ao comando (rótulo, atalho, ativo, marcado).
    void menuItem(const std::string& id);

    static std::string shortcutText(const Shortcut& s);
    std::string shortcutText(const EditorCommand& cmd) const;

private:
    std::vector<EditorCommand> m_commands;
};

} // namespace eruption
