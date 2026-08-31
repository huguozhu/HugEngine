// Panels/PromptPanel.h — 提示词面板（AIGC 场景生成入口）
#pragma once

#include "Core/Types.h"

namespace he::ai::aigc {
class AIPipeline;
} // namespace he::ai::aigc

namespace he::editor {
class GenerationQueuePanel;

/// 提示词面板：输入一句话 → 提交 AIPipeline 后台生成 → 结果入待接受队列
class PromptPanel {
public:
    bool m_Visible = true;

    /// 注入生成管线与待接受队列（由 EditorApp 接线）
    void SetTargets(he::ai::aigc::AIPipeline* pipeline, GenerationQueuePanel* queue) {
        m_Pipeline = pipeline;
        m_Queue = queue;
    }

    void Render();

private:
    he::ai::aigc::AIPipeline* m_Pipeline = nullptr;  // 异步生成管线（不拥有）
    GenerationQueuePanel* m_Queue = nullptr;         // 待接受队列（不拥有）
    char m_PromptBuf[2048] = "一个黄昏下的中世纪村庄，几间石屋、一口井和一盏温暖的篝火";  // 输入缓冲
    bool m_Generating = false;                        // 生成中标志（防重复提交）
    String m_LastError;                               // 最近一次错误信息
};

} // namespace he::editor
