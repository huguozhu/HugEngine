#include "AI/Agent/LLMBrain.h"

#include "AI/Agent/MemoryComponent.h"
#include "AI/Runtime/AIDevice.h"
#include "AI/PromptToScene.h"   // ExtractSceneJsonFromResponse（复用响应提取）
#include "Core/Log.h"

#include "nlohmann/json.hpp"

using nlohmann::json;

namespace he::ai {

LLMBrain::LLMBrain(IAIDevice* device, String systemPrompt)
    : m_Device(device), m_SystemPrompt(std::move(systemPrompt)) {}

String LLMBrain::BuildDecisionPrompt(const String& observationJson,
                                     const MemoryComponent* memory) {
    String p = "你是场景内智能体。依据当前观察与记忆，输出要执行的动作。\n";
    p += "输出必须是合法 JSON，格式：{\"actions\":[{\"op\":\"SpawnEntity\"|\"SetTransform\"|\"SetProperty\",";
    p += "\"targetEntity\":0,\"argsJson\":{...}}]}\n\n";
    p += "可用动作 op 与参数：\n";
    p += "- SpawnEntity: argsJson={\"name\":\"...\",\"transform\":{\"position\":[x,y,z]},\"components\":[{\"type\":\"Cube|Sphere|DirectionalLight|PointLight|PhysicalSky\",...}]}\n";
    p += "- SetTransform: targetEntity=实体id, argsJson={\"position\":[x,y,z],\"scale\":[sx,sy,sz]}\n";
    p += "- SetProperty: targetEntity=实体id, argsJson={\"component\":\"he::TransformComponent\",\"property\":\"position\",\"value\":[x,y,z]}\n\n";
    p += "当前观察（JSON）：\n" + observationJson + "\n";
    if (memory && memory->GetShortTermCount() > 0) {
        // MVP：只输出记忆条目数（值内容较多，避免撑爆 prompt）
        p += "当前记忆条目数：" + std::to_string(memory->GetShortTermCount()) + "\n";
    }
    p += "\n只输出 JSON，不要输出解释文字。";
    return p;
}

ActionPlan LLMBrain::Decide(const String& observationJson, const MemoryComponent* memory) {
    ActionPlan plan;
    if (!m_Device) {
        HE_CORE_WARN("[LLMBrain] 推理设备不可用，无法决策");
        return plan;
    }

    // 1. 拼决策 prompt 并调用 LLM
    String userPrompt = BuildDecisionPrompt(observationJson, memory);
    String response = m_Device->Chat(m_SystemPrompt, userPrompt);

    // 2. 提取动作 JSON 文本（复用 OpenAI 兼容响应解析）
    String content = ExtractSceneJsonFromResponse(response);
    if (content.empty()) {
        HE_CORE_WARN("[LLMBrain] LLM 响应无有效内容");
        return plan;
    }

    // 3. 解析动作列表（非法动作静默跳过，不产生副作用）
    try {
        json root = json::parse(content);
        if (!root.contains("actions") || !root["actions"].is_array()) return plan;
        for (auto& a : root["actions"]) {
            Action act;
            act.targetEntity = a.value("targetEntity", 0ull);
            act.op = a.value("op", String{});
            if (a.contains("argsJson"))
                act.argsJson = a["argsJson"].dump();
            else
                act.argsJson = a.contains("args") ? a["args"].dump() : "{}";
            if (!act.op.empty()) plan.actions.push_back(std::move(act));
        }
    } catch (const std::exception& e) {
        HE_CORE_WARN("[LLMBrain] 动作 JSON 解析失败: {}", e.what());
    }
    HE_CORE_INFO("[LLMBrain] 决策产出 {} 个动作", plan.actions.size());
    return plan;
}

} // namespace he::ai
