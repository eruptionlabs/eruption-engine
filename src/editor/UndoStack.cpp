#include "editor/UndoStack.hpp"

#include <memory>

namespace eruption {

void UndoStack::beginGroup(const std::string& label) {
    if (m_groupDepth++ == 0) {
        m_groupLabel = label;
        m_group.clear();
    }
}

void UndoStack::endGroup() {
    if (m_groupDepth == 0 || --m_groupDepth > 0) return;
    std::vector<Action> group = std::move(m_group);
    m_group.clear();
    if (group.empty()) return;
    if (group.size() == 1) {
        push(std::move(group.front()));
        return;
    }
    auto shared = std::make_shared<std::vector<Action>>(std::move(group));
    push({m_groupLabel,
          [shared] {
              for (auto it = shared->rbegin(); it != shared->rend(); ++it)
                  if (it->undo) it->undo();
          },
          [shared] {
              for (auto& a : *shared)
                  if (a.redo) a.redo();
          }});
}

void UndoStack::push(Action action) {
    if (m_groupDepth > 0) {
        m_group.push_back(std::move(action));
        return;
    }
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
