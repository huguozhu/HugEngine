#pragma once

#include "Core/Types.h"
#include <vector>
#include <string>

namespace he::render {

class ProfilerManager;

// ============================================================
// ProfilerPanel — ImGui GPU Profiler 可视化面板
//
// 提供三种视图模式：
//   1. Timeline: 水平条状图显示每 Pass GPU 耗时
//   2. Heatmap:  颜色编码表格（绿→黄→红）
//   3. History:  帧历史折线图（最后 N 帧总耗时 + 关键 Pass）
//
// 绑定 ProfilerManager 数据源
// 每帧在 ImGui::NewFrame/EndFrame 之间调用 Draw()
// ============================================================
class ProfilerPanel {
public:
    ProfilerPanel();
    ~ProfilerPanel();

    void SetProfiler(ProfilerManager* profiler) { m_Profiler = profiler; }
    void SetVisible(bool visible) { m_Visible = visible; }
    bool IsVisible() const { return m_Visible; }
    void Toggle() { m_Visible = !m_Visible; }

    /// 每帧绘制 ImGui 面板（在 ImGui::NewFrame 之后、EndFrame 之前调用）
    void Draw();

    // 导出为 CSV
    void ExportToCSV(const char* filename) const;

private:
    void DrawTimeline();    // 水平时间轴
    void DrawHeatmap();     // 颜色编码表
    void DrawHistory();     // 帧历史折线图
    void DrawControlBar();  // 顶部控制栏
    void DrawPassTooltip(u32 passIdx) const;

    ProfilerManager* m_Profiler = nullptr;
    bool  m_Visible = false;
    int   m_ViewMode = 0;           // 0=Timeline, 1=Heatmap, 2=History
    int   m_HistoryFrames = 120;    // 历史帧数
    float m_MaxDisplayMs = 16.6f;   // 时间轴最大显示毫秒

    // 帧历史缓存（用于 History 视图）
    struct FrameSnapshot {
        float totalMs = 0.0f;
        std::vector<float> passMs;  // 关键 Pass 的耗时
    };
    mutable std::vector<FrameSnapshot> m_FrameHistory;
    int  m_HistoryFrameCount = 0;
    bool m_HasWarnedBudget = false;
};

} // namespace he::render
