// Panels/GenerationQueuePanel.h — 生成队列面板（待接受/拒绝）
#pragma once

#include "Core/Types.h"
#include <vector>
#include <utility>

namespace he {
class World;
class SceneGraph;
class CommandHistory;
} // namespace he

namespace he::editor {

/// 生成队列面板：展示 AIGC 生成结果（场景规格），
/// 用户「接受」= 经 CommandHistory.Execute(GenerateSceneCommand) 装配（可撤销），
/// 「拒绝」= 丢弃。
class GenerationQueuePanel {
public:
    bool m_Visible = true;

    /// 注入场景与命令历史（接受时装配实体，不拥有指针）
    void Initialize(he::World* world, he::SceneGraph* sg, he::CommandHistory* history) {
        m_World = world;
        m_SG = sg;
        m_History = history;
    }

    /// 入待接受队列（由 PromptPanel 完成回调调用，主线程）
    void AddPending(String prompt, String sceneJson);

    void Render();

private:
    struct Pending {
        String prompt;      // 原始提示词
        String sceneJson;   // 场景规格（LLM 输出）
    };

    he::World*          m_World   = nullptr;   // 装配落点（不拥有）
    he::SceneGraph*     m_SG      = nullptr;   // 场景图（不拥有）
    he::CommandHistory* m_History = nullptr;   // 命令历史（不拥有）

    std::vector<Pending> m_Queue;              // 待接受队列
};

} // namespace he::editor
