#include "AI/AIGC/GenerativeAssetFactory.h"

#include "AI/Runtime/AIDevice.h"
#include "AI/PromptToScene.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Core/Log.h"

namespace he::ai::aigc {

TextureGenResult GenerativeAssetFactory::TextToTexture(he::ai::IAIDevice& device,
                                                      const String& prompt,
                                                      const String& outPath) {
    TextureGenResult result;

    // 1. LLM 输出纹理规格 JSON（注入规格词表约束字段）
    HE_CORE_INFO("[AIGC] 请求生成纹理: {}", prompt);
    String response = device.Chat(TextureGenerator::BuildSpecPrompt(), prompt);
    String content  = ExtractSceneJsonFromResponse(response);
    if (content.empty()) {
        result.error = "LLM 无有效响应（设备/网络不可用）";
        return result;
    }
    HE_CORE_INFO("[AIGC] 纹理规格: {}", content);

    // 2. 解析规格 → 程序化生成像素
    TextureSpec spec;
    if (!TextureGenerator::ParseSpec(content, spec)) {
        result.error = "纹理规格解析失败";
        return result;
    }
    if (!TextureGenerator::Generate(spec, result.pixels)) {
        result.error = "像素生成失败";
        return result;
    }
    result.width  = spec.size;
    result.height = spec.size;

    // 3. 写盘为标准资产文件（PNG，与 glTF 导入的纹理同源）
    if (!outPath.empty()) {
        if (!TextureGenerator::WritePNG(outPath, result.width, result.height,
                                        result.pixels.data())) {
            result.error = "PNG 写盘失败: " + outPath;
            return result;
        }
        result.path = outPath;
    }

    result.success = true;
    return result;
}

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
