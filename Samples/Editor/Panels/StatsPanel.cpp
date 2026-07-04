// Panels/StatsPanel.cpp — 性能统计面板实现
#include "StatsPanel.h"
#include "imgui.h"
#include <algorithm>

namespace he::editor {

void StatsPanel::Render(float deltaTime, u32 drawCalls, u32 triCount) {
    if (!m_Visible) return;

    ImGui::SetNextWindowSize(ImVec2(280, 200), ImGuiCond_FirstUseEver);
    ImGui::Begin("Stats", &m_Visible);

    float fps = deltaTime > 0.0001f ? 1.0f / deltaTime : 0.0f;
    float frameMs = deltaTime * 1000.0f;

    // FPS
    ImGui::TextColored({0.3f, 1.0f, 0.3f, 1.0f}, "FPS: %.0f", fps);
    ImGui::SameLine(80);
    ImGui::Text("(%.2f ms)", frameMs);

    // 帧时间历史图
    if (m_FrameTimes.size() < kHistorySize)
        m_FrameTimes.resize(kHistorySize, frameMs);
    m_FrameTimes[m_FrameIdx % kHistorySize] = frameMs;
    m_FrameIdx++;

    float minT = *std::min_element(m_FrameTimes.begin(), m_FrameTimes.end());
    float maxT = *std::max_element(m_FrameTimes.begin(), m_FrameTimes.end());
    if (maxT - minT < 0.1f) maxT = minT + 16.7f;  // 至少显示 60fps 范围

    char overlay[32];
    snprintf(overlay, sizeof(overlay), "%.1f ms", frameMs);
    ImGui::PlotLines("##FrameTime", m_FrameTimes.data(), (int)m_FrameTimes.size(),
                     m_FrameIdx % kHistorySize, overlay,
                     minT, maxT, ImVec2(-1, 60));

    ImGui::Separator();

    // Draw Calls
    ImGui::Text("Draw Calls:  %u", drawCalls);
    ImGui::Text("Triangles:   %u", triCount);

    ImGui::End();
}

} // namespace he::editor
