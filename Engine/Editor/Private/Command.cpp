#include "Editor/Command.h"
#include "Core/Log.h"

namespace he {

void CommandHistory::Execute(std::unique_ptr<Command> cmd) {
    if (!cmd) return;

    cmd->Execute();
    m_UndoStack.push_back(std::move(cmd));

    // 限制历史深度
    while (m_UndoStack.size() > kMaxHistory) {
        m_UndoStack.pop_front();
    }

    // 新命令清除重做栈
    m_RedoStack.clear();
}

void CommandHistory::Undo() {
    if (m_UndoStack.empty()) return;

    auto cmd = std::move(m_UndoStack.back());
    m_UndoStack.pop_back();

    cmd->Undo();
    m_RedoStack.push_back(std::move(cmd));
}

void CommandHistory::Redo() {
    if (m_RedoStack.empty()) return;

    auto cmd = std::move(m_RedoStack.back());
    m_RedoStack.pop_back();

    cmd->Execute();
    m_UndoStack.push_back(std::move(cmd));
}

void CommandHistory::Clear() {
    m_UndoStack.clear();
    m_RedoStack.clear();
}

} // namespace he
