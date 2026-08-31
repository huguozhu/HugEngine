#pragma once

#include "AI/Agent/Brain.h"

// ============================================================
// LLMBrain — 大模型大脑（经 IAIDevice 的 RemoteBackend）
//
// Decide：观察 + 记忆 + 工具词表 → LLM → 动作 JSON → ActionPlan。
// LLM 只输出结构化动作，执行交给 CompileAction（可撤销、可审查）。
// ============================================================

namespace he::ai {
class IAIDevice;
} // namespace he::ai

namespace he::ai {

class LLMBrain : public IBrain {
public:
    /// @param device 推理设备（RemoteBackend 承载 LLM，可空）
    /// @param systemPrompt 系统提示词（附加观察/记忆/工具说明）
    LLMBrain(IAIDevice* device, String systemPrompt);

    /// 依据观察 + 记忆决策，LLM 输出 {"actions":[...]} 解析为 ActionPlan
    ActionPlan Decide(const String& observationJson, const MemoryComponent* memory) override;

    /// 拼接决策用的 system prompt（观察 + 记忆 + 动作格式说明）
    static String BuildDecisionPrompt(const String& observationJson, const MemoryComponent* memory);

private:
    IAIDevice* m_Device = nullptr;   // 推理设备（不拥有）
    String     m_SystemPrompt;       // 用户提供的系统提示词
};

} // namespace he::ai
