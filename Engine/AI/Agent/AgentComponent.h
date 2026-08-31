#pragma once

#include "Scene/Component.h"
#include "Core/Types.h"

// ============================================================
// AgentComponent — 一等公民智能体组件
//
// 挂载到 Entity 上与 MeshComponent 平级，享受同样的
// 反射/序列化/编辑器内省。由 AgentSystem 按 thinkInterval
// 驱动 IBrain 决策 → Action → Command（可撤销）。
// ============================================================

namespace he::ai {

/// 智能体组件：挂在实体上，声明大脑策略与思考节律
class AgentComponent : public he::Component {
    HE_COMPONENT()
public:
    String brainType      = "LLM";   // 策略类型（LLM / Mock / RL，反射工厂构造 IBrain）
    String systemPrompt;             // 系统提示词（LLM 策略用）
    f32    thinkInterval  = 0.5f;    // 思考间隔（秒）
    bool   enabled        = true;    // 是否启用

    // --- 运行时状态（不注册为反射属性）---
    f32    m_ThinkTimer   = 0.0f;    // 距上次思考的计时
};

} // namespace he::ai
