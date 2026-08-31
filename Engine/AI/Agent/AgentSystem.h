#pragma once

#include "Core/Types.h"

// ============================================================
// AgentSystem — 智能体运行时驱动
//
// 每帧遍历所有 AgentComponent：
//  计时 → 到 thinkInterval 触发 → 构造观察（WorldModel::Snapshot）
//  → 按 brainType 构造大脑（LLMBrain / MockBrain）
//  → Decide 产出 ActionPlan → CompileAction 编译为 he::Command
//  → CommandHistory.Execute（可撤销）
// ============================================================

namespace he {
class World;
class SceneGraph;
class CommandHistory;
} // namespace he

namespace he::ai {
class IAIDevice;

class AgentSystem {
public:
    /// 每帧驱动所有智能体
    /// @param device 推理设备（LLM 大脑需要；Mock 大脑可空）
    /// @param dt 帧间隔（秒）
    static void Update(he::World& world, he::SceneGraph& sg,
                       he::CommandHistory& history, IAIDevice* device, f32 dt);
};

} // namespace he::ai
