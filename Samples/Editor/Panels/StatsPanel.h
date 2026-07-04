// Panels/StatsPanel.h — 性能统计面板
#pragma once

#include "Core/Types.h"
#include <vector>

namespace he::editor {

class StatsPanel {
public:
    bool m_Visible = true;

    void Render(float deltaTime, u32 drawCalls, u32 triCount);

private:
    static constexpr int kHistorySize = 120;
    std::vector<float> m_FrameTimes;
    int m_FrameIdx = 0;
};

} // namespace he::editor
