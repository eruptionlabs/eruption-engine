#include "editor/UndoStack.hpp"

namespace eruption {

void UndoStack::push(Action action) {
    m_actions.resize(m_cursor);
    m_actions.push_back(std::move(action));
    if (m_actions.size() > kMaxActions) m_actions.erase(m_actions.begin());
    m_cursor = m_actions.size();
}

bool UndoStack::undo() {
    if (!canUndo()) return false;
    --m_cursor;
    if (m_actions[m_cursor].undo) m_actions[m_cursor].undo();
    return true;
}

bool UndoStack::redo() {
    if (!canRedo()) return false;
    if (m_actions[m_cursor].redo) m_actions[m_cursor].redo();
    ++m_cursor;
    return true;
}

void UndoStack::clear() {
    m_actions.clear();
    m_cursor = 0;
}

const std::string& UndoStack::undoLabel() const {
    static const std::string kEmpty;
    return canUndo() ? m_actions[m_cursor - 1].label : kEmpty;
}

const std::string& UndoStack::redoLabel() const {
    static const std::string kEmpty;
    return canRedo() ? m_actions[m_cursor].label : kEmpty;
}

} // namespace eruption
