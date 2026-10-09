#pragma once

#include <functional>
#include <string>
#include <vector>

namespace eruption {

// Pilha de desfazer/refazer. Cada ação guarda as duas direções como funções;
// quem edita registra a ação depois de aplicar a mudança.
class UndoStack {
public:
    struct Action {
        std::string label;
        std::function<void()> undo;
        std::function<void()> redo;
    };

    void push(Action action);
    bool undo();
    bool redo();
    void clear();

    bool canUndo() const { return m_cursor > 0; }
    bool canRedo() const { return m_cursor < m_actions.size(); }
    const std::string& undoLabel() const;
    const std::string& redoLabel() const;

private:
    static constexpr size_t kMaxActions = 512;
    std::vector<Action> m_actions;
    size_t m_cursor = 0; // ações [0, cursor) estão aplicadas
};

} // namespace eruption
