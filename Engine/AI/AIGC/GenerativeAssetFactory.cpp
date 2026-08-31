#include "AI/AIGC/GenerativeAssetFactory.h"

#include "AI/Runtime/AIDevice.h"
#include "AI/PromptToScene.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Core/Log.h"

namespace he::ai::aigc {

String GenerativeAssetFactory::GenerateSceneJson(he::ai::IAIDevice& device,
                                                 const String& prompt) {
    // 1. LLM 生成场景 JSON（system prompt 注入组件词表）
    HE_CORE_INFO("[AIGC] 请求生成场景: {}", prompt);
    String response = device.Chat(BuildSceneSystemPrompt(), prompt);
    if (response.empty()) return {};

    // 2. 只提取场景规格文本（不装配实体；装配由接受命令负责）
    return ExtractSceneJsonFromResponse(response);
}

GenerationResult GenerativeAssetFactory::GenerateScene(World& world, SceneGraph& sg,
                                                       he::ai::IAIDevice& device,
                                                       const String& prompt) {
    GenerationResult result;

    // 1. 生成场景规格
    String sceneJson = GenerateSceneJson(device, prompt);
    if (sceneJson.empty()) {
        result.error = "LLM 无响应或响应无效（设备/网络不可用）";
        return result;
    }

    // 2. 立即装配实体树（同步场景生成路径）
    he::ai::SceneBuildResult build = BuildScene(world, sg, sceneJson);
    result.success = build.success;
    result.error   = build.error;
    result.entities = std::move(build.entities);
    return result;
}

} // namespace he::ai::aigc
