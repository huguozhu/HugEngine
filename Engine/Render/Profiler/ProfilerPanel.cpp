// Profiler/ProfilerPanel.cpp — ImGui GPU Profiler 可视化面板
//
// 三种视图模式:
//   Timeline: 水平条状图显示每 Pass GPU 耗时，嵌套缩进
//   Heatmap:  颜色编码表格（绿→黄→红渐变）
//   History:  帧历史折线图（总耗时 + 关键 Pass）
//
// 依赖: ImGui（项目已集成 imgui.h + ImGuiIntegration）
//
#include "ProfilerPanel.h"
#include "ProfilerManager.h"
#include "imgui.h"
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace he::render {

// ── ImGui 颜色辅助 ──
static ImU32 LerpColor(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto lerpByte = [](u8 x, u8 y, float t) -> u8 {
        return static_cast<u8>(x + (y - x) * t);
    };
    return IM_COL32(
        lerpByte((a >>  0) & 0xFF, (b >>  0) & 0xFF, t),
        lerpByte((a >>  8) & 0xFF, (b >>  8) & 0xFF, t),
        lerpByte((a >> 16) & 0xFF, (b >> 16) & 0xFF, t),
        lerpByte((a >> 24) & 0xFF, (b >> 24) & 0xFF, t));
}

// 将 GPU 耗时映射到颜色（0ms=绿 → 16ms=红 → 33ms=暗红）
static ImU32 MsToColor(float ms) {
    float t = std::clamp(ms / 16.6f, 0.0f, 2.0f);
    ImU32 green = IM_COL32(76,  175, 80,  255);
    ImU32 red   = IM_COL32(244, 67,  54,  255);
    ImU32 dark  = IM_COL32(120, 20,  20,  255);
    if (t <= 1.0f) return LerpColor(green, red, t);
    else           return LerpColor(red,   dark, t - 1.0f);
}

ProfilerPanel::ProfilerPanel()  = default;
ProfilerPanel::~ProfilerPanel() = default;

// ============================================================
// Draw — 主入口
// ============================================================
void ProfilerPanel::Draw() {
    if (!m_Visible || !m_Profiler) return;

    // 窗口标题显示帧总耗时
    char title[128];
    snprintf(title, sizeof(title), "GPU Profiler  %.2f ms###ProfilerPanel",
             m_Profiler->GetTotalFrameMs());

    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &m_Visible, ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    DrawControlBar();

    switch (m_ViewMode) {
        case 0: DrawTimeline(); break;
        case 1: DrawHeatmap();  break;
        case 2: DrawHistory();  break;
    }

    // 帧预算警告
    if (!m_HasWarnedBudget) {
        auto warns = m_Profiler->CheckBudgets();
        for (auto& w : warns) {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", w.c_str());
        }
    }

    ImGui::End();
}

// ============================================================
// DrawControlBar — 顶部模式切换 + 导出按钮
// ============================================================
void ProfilerPanel::DrawControlBar() {
    if (ImGui::Button("Timeline"))  m_ViewMode = 0;
    ImGui::SameLine();
    if (ImGui::Button("Heatmap"))   m_ViewMode = 1;
    ImGui::SameLine();
    if (ImGui::Button("History"))   m_ViewMode = 2;
    ImGui::SameLine();
    if (ImGui::Button("Export CSV")) ExportToCSV("profiler_export.csv");
    ImGui::SameLine();
    ImGui::Text("| 延迟 2 帧");

    // 状态信息
    ImGui::SameLine();
    bool statsOn = m_Profiler->IsPipelineStatsEnabled();
    ImGui::TextColored(statsOn ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1),
                       statsOn ? "Stats ON" : "Stats OFF");
    ImGui::Separator();
}

// ============================================================
// DrawTimeline — 水平条状图
// ============================================================
void ProfilerPanel::DrawTimeline() {
    auto& scopes = m_Profiler->GetLastFrameScopes();
    if (scopes.empty()) { ImGui::Text("无数据"); return; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    float rowH = 18.0f;
    float indentW = 12.0f;

    // 绘制刻度线
    for (float ms : {0.0f, 4.0f, 8.0f, 12.0f, 16.6f, 33.3f}) {
        float x = pos.x + (ms / m_MaxDisplayMs) * availW;
        dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + 40), IM_COL32(80, 80, 80, 255));
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0fms", ms);
        dl->AddText(ImVec2(x + 2, pos.y), IM_COL32(200, 200, 200, 255), buf);
    }

    // 16.6ms 帧预算参考线
    float budgetX = pos.x + (16.6f / m_MaxDisplayMs) * availW;
    dl->AddLine(ImVec2(budgetX, pos.y + 40), ImVec2(budgetX, pos.y + 40 + scopes.size() * rowH),
                IM_COL32(255, 100, 100, 100));

    pos.y += 42;

    for (usize i = 0; i < scopes.size(); ++i) {
        auto& s = scopes[i];
        if (s.gpuMs < 0) continue;

        float barW = std::max((s.gpuMs / m_MaxDisplayMs) * availW, 3.0f);
        float indent = s.depth * indentW;
        ImVec2 barPos(pos.x + indent, pos.y + 2);
        ImVec2 barSize(barW, rowH - 4);

        // 背景条
        ImU32 color = MsToColor(s.gpuMs);
        dl->AddRectFilled(barPos, ImVec2(barPos.x + barSize.x, barPos.y + barSize.y), color);

        // Pass 名称 + 耗时
        char label[128];
        snprintf(label, sizeof(label), "%s  %.2f ms", s.name.c_str(), s.gpuMs);
        dl->AddText(ImVec2(barPos.x + 4, barPos.y), IM_COL32(255, 255, 255, 255), label);

        // 鼠标悬停详情
        ImVec2 hoverMin = barPos;
        ImVec2 hoverMax(barPos.x + barSize.x + 200, barPos.y + barSize.y);
        if (ImGui::IsMouseHoveringRect(hoverMin, hoverMax)) {
            ImGui::BeginTooltip();
            ImGui::Text("%s: %.3f ms", s.name.c_str(), s.gpuMs);
            if (m_Profiler->HasPipelineStats(static_cast<u32>(i))) {
                auto& st = s.stats;
                ImGui::Text("  Vertices: %llu  |  Primitives: %llu",
                            (unsigned long long)st.inputVertices,
                            (unsigned long long)st.inputPrimitives);
                ImGui::Text("  VS calls: %llu  |  PS calls: %llu",
                            (unsigned long long)st.vsInvocations,
                            (unsigned long long)st.fsInvocations);
                ImGui::Text("  Overdraw: %.1f  |  CullRate: %.1f%%",
                            st.GetVsToFsRatio(), st.GetPrimitiveCullRate() * 100.0f);
            }
            ImGui::EndTooltip();
        }

        pos.y += rowH;
    }
}

// ============================================================
// DrawHeatmap — 颜色编码表格
// ============================================================
void ProfilerPanel::DrawHeatmap() {
    auto& scopes = m_Profiler->GetLastFrameScopes();
    if (scopes.empty()) { ImGui::Text("无数据"); return; }

    if (ImGui::BeginTable("##heatmap", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("GPU (ms)", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("%% 帧时间", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();

        float total = m_Profiler->GetTotalFrameMs();

        for (usize i = 0; i < scopes.size(); ++i) {
            auto& s = scopes[i];
            if (s.gpuMs < 0) continue;

            ImGui::TableNextRow();

            // Pass 名称（含缩进）
            ImGui::TableSetColumnIndex(0);
            std::string indent(s.depth * 2, ' ');
            ImGui::Text("%s%s", indent.c_str(), s.name.c_str());

            // GPU 耗时（颜色编码背景）
            ImGui::TableSetColumnIndex(1);
            ImU32 bg = MsToColor(s.gpuMs);
            ImVec2 cellMin = ImGui::GetCursorScreenPos();
            ImVec2 cellMax(cellMin.x + 100, cellMin.y + ImGui::GetTextLineHeight());
            ImGui::GetWindowDrawList()->AddRectFilled(cellMin, cellMax, bg);
            ImGui::Text("%.3f", s.gpuMs);

            // 帧占比
            ImGui::TableSetColumnIndex(2);
            float pct = total > 0 ? (s.gpuMs / total) * 100.0f : 0;
            ImGui::Text("%.1f%%", pct);
        }
        ImGui::EndTable();
    }

    // 底栏汇总
    ImGui::Separator();
    ImGui::Text("总帧耗时: %.2f ms  |  Pass 数量: %zu",
                m_Profiler->GetTotalFrameMs(), scopes.size());
}

// ============================================================
// DrawHistory — 帧历史折线图
// ============================================================
void ProfilerPanel::DrawHistory() {
    // 收集帧快照
    float totalMs = m_Profiler->GetTotalFrameMs();
    FrameSnapshot snap;
    snap.totalMs = totalMs;
    auto& scopes = m_Profiler->GetLastFrameScopes();
    for (auto& s : scopes) {
        if (s.gpuMs >= 0) snap.passMs.push_back(s.gpuMs);
    }
    m_FrameHistory.push_back(snap);
    m_HistoryFrameCount++;

    // 限制历史帧数
    while (static_cast<int>(m_FrameHistory.size()) > m_HistoryFrames)
        m_FrameHistory.erase(m_FrameHistory.begin());

    ImGui::SliderInt("历史帧数", &m_HistoryFrames, 60, 600);
    ImGui::SameLine();
    ImGui::Text("总帧: %d", m_HistoryFrameCount);

    // 构建总帧耗时数组
    std::vector<float> totalHistory;
    for (auto& fs : m_FrameHistory) totalHistory.push_back(fs.totalMs);

    float maxVal = 0;
    for (float v : totalHistory) maxVal = std::max(maxVal, v);
    maxVal = std::max(maxVal, 33.3f);

    char overlay[64];
    snprintf(overlay, sizeof(overlay), "avg=%.2fms  max=%.2fms  min=%.2fms",
             totalMs, maxVal,
             totalHistory.empty() ? 0.0f :
             *std::min_element(totalHistory.begin(), totalHistory.end()));

    ImGui::PlotLines("##frameTotal", totalHistory.data(),
                     static_cast<int>(totalHistory.size()), 0,
                     overlay, 0.0f, maxVal,
                     ImVec2(ImGui::GetContentRegionAvail().x, 150));

    ImGui::Separator();
    ImGui::Text("帧 %d | GPU %.2f ms", m_HistoryFrameCount, totalMs);
}

// ============================================================
// ExportToCSV — 导出当前帧数据
// ============================================================
void ProfilerPanel::ExportToCSV(const char* filename) const {
    if (!m_Profiler) return;

    FILE* f = fopen(filename, "w");
    if (!f) return;

    auto& scopes = m_Profiler->GetLastFrameScopes();
    fprintf(f, "Pass,GPU_ms,Vertices,Primitives,VS_calls,PS_calls,CullRate,Overdraw\n");
    for (auto& s : scopes) {
        if (s.gpuMs < 0) continue;
        fprintf(f, "%s,%.3f,%llu,%llu,%llu,%llu,%.2f,%.1f\n",
                s.name.c_str(), s.gpuMs,
                (unsigned long long)s.stats.inputVertices,
                (unsigned long long)s.stats.inputPrimitives,
                (unsigned long long)s.stats.vsInvocations,
                (unsigned long long)s.stats.fsInvocations,
                s.stats.GetPrimitiveCullRate(),
                s.stats.GetVsToFsRatio());
    }
    fclose(f);
}

// ============================================================
// DrawPassTooltip — Pass 详情浮窗（预留）
// ============================================================
void ProfilerPanel::DrawPassTooltip(u32 passIdx) const {
    (void)passIdx; // 当前在 Timeline 的悬停处理中实现
}

} // namespace he::render
