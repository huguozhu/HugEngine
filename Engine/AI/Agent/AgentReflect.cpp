#include "AI/Agent/AgentComponent.h"
#include "AI/Agent/MemoryComponent.h"
#include "AI/Agent/GoalComponent.h"
#include "Reflect/ReflectionMacros.h"
#include "Reflect/Attribute.h"

// ============================================================
// AgentReflect.cpp — AI 智能体组件反射注册
//
// 注意：注册放在 AI 模块内（而非 Scene 的 SceneReflect.cpp），
// 避免 Scene → AI 的反向依赖。
// ============================================================

namespace he::ai {

// --- AgentComponent 注册（带 AI 注解示范）---
HE_BEGIN_REGISTER(he::ai::AgentComponent)
    HE_REGISTER_PROPERTY(he::ai::AgentComponent, String, brainType)
        HE_ATTR_CATEGORY("AI") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("大脑策略类型：LLM/Mock/RL")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::ai::AgentComponent, String, systemPrompt)
        HE_ATTR_CATEGORY("AI") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("系统提示词")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::ai::AgentComponent, f32, thinkInterval)
        HE_ATTR_CATEGORY("AI") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("思考间隔（秒）")
    HE_END_PROPERTY()
    HE_REGISTER_PROPERTY(he::ai::AgentComponent, bool, enabled)
        HE_ATTR_CATEGORY("AI") HE_ATTR_AI_VISIBLE() HE_ATTR_AI_WRITABLE() HE_ATTR_AI_DESCRIPTION("是否启用")
    HE_END_PROPERTY()
HE_END_REGISTER()

// --- MemoryComponent / GoalComponent：组件类型注册（内部数据结构暂不注册属性）---
HE_BEGIN_REGISTER(he::ai::MemoryComponent)
HE_END_REGISTER()

HE_BEGIN_REGISTER(he::ai::GoalComponent)
HE_END_REGISTER()

} // namespace he::ai
