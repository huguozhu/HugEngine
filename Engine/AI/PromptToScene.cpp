#include "AI/PromptToScene.h"

#include "AI/LLMClient.h"
#include "AI/TypeSchema.h"
#include "Core/Log.h"

#include "nlohmann/json.hpp"

using nlohmann::json;

namespace he::ai {

// 拼接 system prompt：角色设定 + 输出格式 + 可用组件词汇表 + 规则
static String BuildSystemPrompt() {
    String p = "你是 3D 场景生成器。根据用户描述，生成一个 JSON 场景描述。\n";
    p += "输出必须是合法 JSON，格式如下：\n";
    p += "{ \"entities\": [ { \"name\":\"...\", \"transform\":{\"position\":[x,y,z],\"scale\":[sx,sy,sz]}, ";
    p += "\"components\":[{\"type\":\"...\", ...字段}] } ] }\n\n";
    p += "可用的组件类型与字段（只能使用这些）：\n";
    p += BuildTypeSchema();
    p += "\n\n规则：\n";
    p += "- 只使用上述组件类型和字段，不要发明新类型。\n";
    p += "- position 单位米，color 是 [r,g,b] 0~1，intensity 是正数。\n";
    p += "- 场景必须包含至少一个光源和一个地面。\n";
    p += "- 只输出 JSON，不要输出任何解释文字。\n";
    return p;
}

SceneBuildResult PromptToScene(ILLMClient& llm, World& world, SceneGraph& sg,
                               const String& prompt) {
    String systemPrompt = BuildSystemPrompt();
    HE_CORE_INFO("[PromptToScene] 请求 LLM 生成场景: {}", prompt);

    // 1. 调 LLM，拿到 OpenAI 兼容响应 JSON 文本
    String response = llm.Chat(systemPrompt, prompt);

    // 2. 解析响应（非法响应直接失败，不产生任何实体）
    json resp;
    try {
        resp = json::parse(response);
    } catch (const std::exception& e) {
        SceneBuildResult r;
        r.error = String("LLM 响应解析失败: ") + e.what();
        return r;
    }
    if (!resp.contains("choices") || !resp["choices"].is_array() || resp["choices"].empty()) {
        SceneBuildResult r;
        r.error = "LLM 响应缺少 choices 字段";
        return r;
    }

    // 3. 取出模型输出的场景 JSON 文本
    String sceneJson = resp["choices"][0]["message"]["content"].get<String>();
    HE_CORE_INFO("[PromptToScene] LLM 返回场景 JSON:\n{}", sceneJson);

    // 4. 交给 SceneBuilder 解释成真实 Entity/Component
    return BuildScene(world, sg, sceneJson);
}

} // namespace he::ai
