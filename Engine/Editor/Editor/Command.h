#pragma once

#include "Core/Types.h"
#include <deque>
#include <memory>
#include <string>

// ============================================================
// Command — Undo/Redo 命令系统
//
// 用法:
//   class MoveEntityCommand : public Command {
//       void Execute() override { entity->position = newPos; }
//       void Undo()    override { entity->position = oldPos; }
//   };
//   CommandHistory history;
//   history.Execute(std::make_unique<MoveEntityCommand>(...));
//   history.Undo();  // 撤销
//   history.Redo();  // 重做
// ============================================================

namespace he {

/// 可撤销命令基类
class Command {
public:
    virtual ~Command() = default;
    virtual void Execute() = 0;
    virtual void Undo()    = 0;
    virtual String GetDescription() const { return "Command"; }
};

/// 命令历史管理器（Undo/Redo 栈）
class CommandHistory {
public:
    static constexpr usize kMaxHistory = 256;

    /// 执行命令并推入撤销栈
    void Execute(std::unique_ptr<Command> cmd);

    /// 撤销上一个命令
    void Undo();
    /// 重做上一个撤销的命令
    void Redo();

    /// 是否有可撤销/重做的命令
    bool CanUndo() const { return !m_UndoStack.empty(); }
    bool CanRedo() const { return !m_RedoStack.empty(); }

    /// 清空历史
    void Clear();

private:
    std::deque<std::unique_ptr<Command>> m_UndoStack;
    std::deque<std::unique_ptr<Command>> m_RedoStack;
};

/// 属性修改命令（通过 lambda 捕获旧值/新值，支持任意类型属性的 Undo/Redo）
class PropertyChangeCommand : public Command {
public:
    using Action = std::function<void()>;

    PropertyChangeCommand(String desc, Action undoAction, Action redoAction)
        : m_Desc(std::move(desc)), m_Undo(std::move(undoAction)), m_Redo(std::move(redoAction)) {}

    void Execute() override { m_Redo(); }
    void Undo()    override { m_Undo(); }
    String GetDescription() const override { return m_Desc; }

private:
    String m_Desc;
    Action m_Undo;  // 撤销 = 恢复旧值
    Action m_Redo;  // 重做 = 应用新值
};

} // namespace he
